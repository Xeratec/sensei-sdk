/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_commission.h
 * @brief Adding a node over BLE with a setup code: SPAKE2+ (RFC 9383) on P-256, the verifier record, the kind 5
 *        channel. C99, no Zephyr, no allocation. The Python mirror is sensei_sdk.commission.
 *
 * The node is the verifier: it holds the record (scrypt parameters, salt, w0, L), never the code. The commissioner (a
 * host, a test, a simulator) is the prover. Both use the cipher backend of sensei_secure.h (sha256, scalar_reduce,
 * spake2p_* and the existing hkdf, hmac_sha256, aead_*). Message bodies are plain bytes here: the protobuf layer
 * belongs to the application.
 *
 * Node, in the order of the messages:
 *   begin (commission_begin)  sensei_commission_verifier_begin()   -> nonce_n for commission_params
 *   pake1 (shareP)            sensei_commission_verifier_pake1()   -> shareV, confirmV (the attempt is counted and
 *                                                                     stored before they are given out)
 *   pake3 (confirmP)          sensei_commission_verifier_pake3()   -> the channel is open
 *   kind 5 frames             sensei_commission_verifier_seal() / _open()
 *   abort, finish, any error  sensei_commission_verifier_end()
 * Every call takes the time (ms, any clock that does not go back; unsigned wrap-around is fine) and ends the session
 * with SENSEI_COMMISSION_ERR_TIMEOUT if a limit has passed. Every error of begin, pake1 or pake3 ends the session:
 * the secrets are wiped on every exit. Errors of the channel (a frame that does not open) leave it open.
 *
 * Commissioner: sensei_commission_prover_begin(), _share(), _finish(), then _seal() / _open() and _end().
 * The prover takes w0 and w1 as input: the scrypt over the code (sensei_commission_pwin() is its input, and
 * sensei_commission_derive_w() turns its output into w0 and w1) is not in this library.
 *
 * Random numbers: one call of random() draws nonce_n (16 B); the ephemeral scalar is 40 B reduced mod n (drawn again
 * in the 2^-250 case of 0). Tests give a backend whose random() returns fixed bytes.
 */

#ifndef SENSEI_COMMISSION_H_
#define SENSEI_COMMISSION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensei_secure.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSEI_COMMISSION_SUITE 1u /* P256-SHA256-HKDF-SHA256-HMAC-SHA256 */
#define SENSEI_COMMISSION_KDF_SCRYPT 1u
#define SENSEI_COMMISSION_LOG2N_MIN 14u /* what a record and a commissioner accept */
#define SENSEI_COMMISSION_LOG2N_MAX 17u
#define SENSEI_COMMISSION_SCRYPT_R 8u
#define SENSEI_COMMISSION_SCRYPT_P 1u
#define SENSEI_COMMISSION_SALT_SIZE 16u
#define SENSEI_COMMISSION_NONCE_SIZE 16u
#define SENSEI_COMMISSION_POINT_SIZE 65u   /* shares, M, N, L */
#define SENSEI_COMMISSION_SCALAR_SIZE 32u  /* w0, w1 */
#define SENSEI_COMMISSION_CONFIRM_SIZE 32u /* confirmV, confirmP */
#define SENSEI_COMMISSION_DK_SIZE 80u      /* scrypt output: w0s || w1s */
#define SENSEI_COMMISSION_CONTEXT_SIZE 73u
#define SENSEI_COMMISSION_ID_SIZE 19u      /* both identities */
#define SENSEI_COMMISSION_RECORD_SIZE 121u /* the verifier record */
#define SENSEI_COMMISSION_RECORD_VERSION 1u
#define SENSEI_COMMISSION_FP_SIZE 16u          /* node fingerprint */
#define SENSEI_COMMISSION_COLLECTOR_FP_SIZE 8u /* collector fingerprint */
#define SENSEI_COMMISSION_POP_SIZE 64u         /* plaintext of the proof of possession */
#define SENSEI_COMMISSION_CODE_DIGITS 11u      /* random digits of the setup code */
#define SENSEI_COMMISSION_CODE_SIZE 12u        /* with the Verhoeff check digit */
#define SENSEI_COMMISSION_QR_SIZE 66u          /* characters of the QR payload (without the end of string) */

/* Defaults of struct sensei_commission_limits (design section 1.1). */
#define SENSEI_COMMISSION_BEGIN_MS 30000u    /* begin to pake1 */
#define SENSEI_COMMISSION_PAKE_MS 15000u     /* pake2 sent to pake3 */
#define SENSEI_COMMISSION_IDLE_MS 60000u     /* no frame in the secure phase */
#define SENSEI_COMMISSION_SESSION_MS 180000u /* begin to the end, whatever happens */
#define SENSEI_COMMISSION_FAILS_MAX 30u      /* lockout: fails_since_ok at this value */

/* Errors: the codes of sensei_secure.h where they fit (FORMAT -1: bad length, encoding, record or an invalid share;
 * AUTH -3: a confirmation that is wrong; STATE -7; CRYPTO -6; STORE -10), and two more. */
enum {
  SENSEI_COMMISSION_ERR_TIMEOUT = -30, /* a time limit passed: the session is ended */
  SENSEI_COMMISSION_ERR_LOCKED = -31,  /* fails_since_ok has reached the maximum: no attempt is started */
};

/** Scrypt parameters and salt: what the commissioner needs to turn the code into w0 and w1, and what goes into the
 * context. */
struct sensei_commission_params {
  uint8_t suite, kdf, log2n, r, p;
  uint8_t salt[SENSEI_COMMISSION_SALT_SIZE];
};

/** The verifier record: what the node keeps for one code. The code is not in it. */
struct sensei_commission_record {
  struct sensei_commission_params params;
  uint8_t w0[SENSEI_COMMISSION_SCALAR_SIZE];
  uint8_t l[SENSEI_COMMISSION_POINT_SIZE]; /* L = w1*G */
};

/** 0 if suite 1, kdf 1 (scrypt), log2n 14 to 17, r 8, p 1; else SENSEI_SECURE_ERR_FORMAT. The salt is any 16 bytes. */
int sensei_commission_params_check(const struct sensei_commission_params *p);

/**
 * Decode the 121 B record: version 1, suite, kdf, log2n, r, p, two zero bytes, salt[16], w0[32], L[65]. Refuses
 * (ERR_FORMAT, nothing written) another length or version, a reserved byte that is not 0, parameters that
 * params_check() refuses, w0 that is not in 1 ... n-1, and an L that is not a point of the curve (backend
 * spake2p_check).
 */
int sensei_commission_record_decode(const struct sensei_secure_crypto *c, const uint8_t *data, size_t len,
                                    struct sensei_commission_record *rec);

/** Encode a record after the same checks (the bench and the USB setup path build one this way). Returns 0 or
 * ERR_FORMAT. */
int sensei_commission_record_encode(const struct sensei_secure_crypto *c, const struct sensei_commission_record *rec,
                                    uint8_t out[SENSEI_COMMISSION_RECORD_SIZE]);

/** Attempt counters, kept in flash by the node (record e2e_cmf). fails_total: attempts that did not end in a confirmed
 * code. fails_since_ok: the same since the last success; it drives the lockout. */
struct sensei_commission_counters {
  uint32_t fails_total;
  uint32_t fails_since_ok;
};

/** Time limits and lockout of a session. A field that is 0 takes its default (SENSEI_COMMISSION_*_MS, _FAILS_MAX). */
struct sensei_commission_limits {
  uint32_t begin_ms;   /* begin to pake1 */
  uint32_t pake_ms;    /* pake2 to pake3 */
  uint32_t idle_ms;    /* idle in the secure phase */
  uint32_t session_ms; /* begin to the end */
  uint32_t fails_max;  /* lockout at fails_since_ok >= fails_max */
};

/** The node's side of one session. All pointers must stay valid until sensei_commission_verifier_end(). */
struct sensei_commission_verifier_cfg {
  const struct sensei_secure_crypto *c;
  const struct sensei_commission_record *record; /* copied at begin */
  uint64_t node_id;                              /* not 0 */
  /** RAM copy of the counters (loaded from flash). It changes only after save_counters() returned 0. */
  struct sensei_commission_counters *counters;
  /** Stores the counters. Called by pake1 with the incremented values before shareV is given out, and by pake3 with the
   * reset values. A nonzero return at pake1 ends the session (ERR_STORE) and the attempt is not started; at pake3 it is
   * ignored (the success stands, the counters stay as they were). Required. */
  int (*save_counters)(void *user, const struct sensei_commission_counters *cnt);
  void *user;
  struct sensei_commission_limits limits;
};

/** The kind 5 channel of one session (both sides). Counters start at 1 and live in RAM; the replay window is 64. */
struct sensei_commission_channel {
  const struct sensei_secure_crypto *c;
  sensei_secure_key_t k_tx, k_rx;
  uint32_t sid;
  uint32_t tx_counter; /* last counter sent */
  bool to_node;        /* direction of what is sent: true for the commissioner */
  bool open;
  struct sensei_secure_replay rx;
};

enum sensei_commission_role {
  SENSEI_COMMISSION_VERIFIER = 0, /* the node: sends k_n2c (direction 0), receives k_c2n */
  SENSEI_COMMISSION_PROVER = 1,   /* the commissioner: sends k_c2n (direction 1, sender 1), receives k_n2c */
};

/** Open a channel from the keys of a session (k_c2n and k_n2c 16 B, sid not 0). 0, ERR_STATE or ERR_CRYPTO. */
int sensei_commission_channel_open(struct sensei_commission_channel *ch, const struct sensei_secure_crypto *c,
                                   enum sensei_commission_role role, const uint8_t k_c2n[16], const uint8_t k_n2c[16],
                                   uint32_t sid);

/** Seal pt as kind 5 with the next counter. Returns the envelope length, ERR_SPACE (over 320 B), ERR_STATE (counter
 * exhausted, channel closed) or ERR_CRYPTO. The counter is used only if it works. */
int sensei_commission_channel_seal(struct sensei_commission_channel *ch, const uint8_t *pt, size_t len, uint8_t *out,
                                   size_t cap);

/** Open a kind 5 frame from the other side (sensei_secure_open_commission() with the window of the channel). 0 or
 * ERR_FORMAT, ERR_REPLAY, ERR_AUTH, ERR_SPACE, ERR_STATE. Nothing changes on failure. */
int sensei_commission_channel_recv(struct sensei_commission_channel *ch, const uint8_t *in, size_t in_len, uint8_t *out,
                                   size_t cap, size_t *out_len);

/** Destroy the key handles and wipe the channel. */
void sensei_commission_channel_close(struct sensei_commission_channel *ch);

enum sensei_commission_state {
  SENSEI_COMMISSION_IDLE = 0, /* not started, or ended */
  SENSEI_COMMISSION_BEGUN,    /* waiting for pake1 (verifier) / share (prover) */
  SENSEI_COMMISSION_SENT,     /* pake2 sent, waiting for pake3 (verifier) / finish (prover) */
  SENSEI_COMMISSION_OPEN,     /* confirmed: the channel is open */
};

/** The node's session. Fill with zeros or leave it uninitialised: begin clears it. About 500 bytes. */
struct sensei_commission_verifier {
  struct sensei_commission_verifier_cfg cfg;
  enum sensei_commission_state state;
  struct sensei_commission_record rec; /* w0 is wiped after pake1 */
  uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE];
  uint8_t nonce_n[SENSEI_COMMISSION_NONCE_SIZE];
  uint8_t confirm_p[SENSEI_COMMISSION_CONFIRM_SIZE]; /* what confirmP must be (from pake1 to pake3) */
  uint8_t k_c2n[16], k_n2c[16];
  uint32_t sid;
  uint32_t t_begin, t_phase;
  struct sensei_commission_limits lim; /* defaults filled in */
  struct sensei_commission_channel ch;
};

/**
 * Start a session. cfg->c needs the commissioning backend, cfg->record is checked (record_decode() rules apply to a
 * record that did not come from there, so check it again), node_id is not 0, nonce_g is the commissioner's nonce.
 * Draws nonce_n (written to nonce_n_out, 16 B: it goes into commission_params). ERR_LOCKED if fails_since_ok has
 * reached the maximum. Another error: ERR_FORMAT (bad record or node id), ERR_STATE (a NULL pointer, no backend
 * support), ERR_CRYPTO. The session is idle after an error.
 */
int sensei_commission_verifier_begin(struct sensei_commission_verifier *v,
                                     const struct sensei_commission_verifier_cfg *cfg,
                                     const uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE], uint32_t now_ms,
                                     uint8_t nonce_n_out[SENSEI_COMMISSION_NONCE_SIZE]);

/**
 * shareP in, shareV and confirmV out. Checks shareP (65 B, prefix 04, on the curve, and shareP - w0*M is not the
 * identity: ERR_FORMAT, no attempt counted), computes everything, then counts the attempt: the counters are
 * incremented, save_counters() is called, and only if it returns 0 are share_v and confirm_v written
 * (ERR_STORE: nothing counted, nothing sent). ERR_LOCKED if the lockout is reached. The session is ended on any error.
 * The secrets (w0, the ephemeral scalar, Z, V, the keys of the schedule) are wiped before it returns.
 */
int sensei_commission_verifier_pake1(struct sensei_commission_verifier *v, uint32_t now_ms,
                                     const uint8_t share_p[SENSEI_COMMISSION_POINT_SIZE],
                                     uint8_t share_v[SENSEI_COMMISSION_POINT_SIZE],
                                     uint8_t confirm_v[SENSEI_COMMISSION_CONFIRM_SIZE]);

/**
 * confirmP in. A wrong value: ERR_AUTH, the session ends and the attempt stays counted. A right one: the counters
 * fails_since_ok = 0 and fails_total - 1 are stored (a failure of the store is ignored), the channel is open. After it
 * the node sends its first data (the identity) and not before.
 */
int sensei_commission_verifier_pake3(struct sensei_commission_verifier *v, uint32_t now_ms,
                                     const uint8_t confirm_p[SENSEI_COMMISSION_CONFIRM_SIZE]);

/** The session id (the key id field of the frames), or 0 before pake3. */
uint32_t sensei_commission_verifier_sid(const struct sensei_commission_verifier *v);

/** Seal / open a kind 5 frame in the secure phase (see the channel functions). A frame that works resets the idle
 * time. The session must be open (ERR_STATE) and in time (ERR_TIMEOUT: ended). */
int sensei_commission_verifier_seal(struct sensei_commission_verifier *v, uint32_t now_ms, const uint8_t *pt,
                                    size_t len, uint8_t *out, size_t cap);
int sensei_commission_verifier_recv(struct sensei_commission_verifier *v, uint32_t now_ms, const uint8_t *in,
                                    size_t in_len, uint8_t *out, size_t cap, size_t *out_len);

/** Check the time without doing anything: 0, or ERR_TIMEOUT after ending the session. An idle session returns 0. */
int sensei_commission_verifier_poll(struct sensei_commission_verifier *v, uint32_t now_ms);

/** The state of the session (the limits are checked by the calls and by poll()). */
enum sensei_commission_state sensei_commission_verifier_state(const struct sensei_commission_verifier *v);

/** End the session: destroy the channel keys and wipe everything. Safe to call at any time and twice. */
void sensei_commission_verifier_end(struct sensei_commission_verifier *v);

/** The commissioner's session. */
struct sensei_commission_prover {
  const struct sensei_secure_crypto *c;
  enum sensei_commission_state state;
  struct sensei_commission_params params;
  uint8_t w0[SENSEI_COMMISSION_SCALAR_SIZE], w1[SENSEI_COMMISSION_SCALAR_SIZE];
  uint8_t x[SENSEI_COMMISSION_SCALAR_SIZE];
  uint8_t ctx[SENSEI_COMMISSION_CONTEXT_SIZE];
  uint8_t id_v[SENSEI_COMMISSION_ID_SIZE];
  uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE];
  uint8_t nonce_n[SENSEI_COMMISSION_NONCE_SIZE];
  uint8_t share_p[SENSEI_COMMISSION_POINT_SIZE];
  uint64_t node_id;
  struct sensei_commission_channel ch;
};

/** w0, w1: scalars in 1 ... n-1 (ERR_FORMAT otherwise); params as the node sent them (params_check() applies);
 * nonce_g and nonce_n of the session. Draws the ephemeral scalar. */
int sensei_commission_prover_begin(struct sensei_commission_prover *p, const struct sensei_secure_crypto *c,
                                   const uint8_t w0[SENSEI_COMMISSION_SCALAR_SIZE],
                                   const uint8_t w1[SENSEI_COMMISSION_SCALAR_SIZE], uint64_t node_id,
                                   const struct sensei_commission_params *params,
                                   const uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE],
                                   const uint8_t nonce_n[SENSEI_COMMISSION_NONCE_SIZE]);

/** shareP = x*G + w0*M (message Pake1). */
int sensei_commission_prover_share(struct sensei_commission_prover *p, uint8_t share_p[SENSEI_COMMISSION_POINT_SIZE]);

/** Check shareV and confirmV (Pake2), write confirmP (Pake3) and open the channel. ERR_AUTH: the node does not know
 * the same code (confirmV wrong); ERR_FORMAT: shareV is not a valid share. The session ends on an error. */
int sensei_commission_prover_finish(struct sensei_commission_prover *p,
                                    const uint8_t share_v[SENSEI_COMMISSION_POINT_SIZE],
                                    const uint8_t confirm_v[SENSEI_COMMISSION_CONFIRM_SIZE],
                                    uint8_t confirm_p[SENSEI_COMMISSION_CONFIRM_SIZE]);

int sensei_commission_prover_seal(struct sensei_commission_prover *p, const uint8_t *pt, size_t len, uint8_t *out,
                                  size_t cap);
int sensei_commission_prover_recv(struct sensei_commission_prover *p, const uint8_t *in, size_t in_len, uint8_t *out,
                                  size_t cap, size_t *out_len);
void sensei_commission_prover_end(struct sensei_commission_prover *p);

/* ---- pieces of the protocol, public for the other side of a test and for the tools ---- */

/** M and N of RFC 9383 section 4 for P-256 (65 B points). */
extern const uint8_t sensei_commission_m[SENSEI_COMMISSION_POINT_SIZE];
extern const uint8_t sensei_commission_n[SENSEI_COMMISSION_POINT_SIZE];

/** Size of the transcript of a SENSEI session (the RFC 9383 test vectors are shorter). */
#define SENSEI_COMMISSION_TT_SIZE 613u

/**
 * The transcript TT of RFC 9383 section 3.3: context, idProver, idVerifier, M, N, shareP, shareV, Z, V, w0, each with
 * its length as 8 B little endian in front. Returns the length, or 0 if cap is too small.
 */
size_t sensei_commission_transcript(uint8_t *out, size_t cap, const uint8_t *ctx, size_t ctx_len, const uint8_t *id_p,
                                    size_t id_p_len, const uint8_t *id_v, size_t id_v_len,
                                    const uint8_t share_p[SENSEI_COMMISSION_POINT_SIZE],
                                    const uint8_t share_v[SENSEI_COMMISSION_POINT_SIZE],
                                    const uint8_t z[SENSEI_COMMISSION_POINT_SIZE],
                                    const uint8_t v[SENSEI_COMMISSION_POINT_SIZE],
                                    const uint8_t w0[SENSEI_COMMISSION_SCALAR_SIZE]);

/** The key schedule of RFC 9383 section 3.4 (SHA-256, HKDF-SHA256 with no salt, HMAC-SHA256). */
struct sensei_commission_schedule {
  uint8_t k_main[32];
  uint8_t k_confirm_p[32];
  uint8_t k_confirm_v[32];
  uint8_t k_shared[32];
  uint8_t confirm_p[32]; /* HMAC(K_confirmP, shareV): sent by the prover */
  uint8_t confirm_v[32]; /* HMAC(K_confirmV, shareP): sent by the verifier */
};

/** K_main = SHA-256(TT); K_confirmP || K_confirmV = HKDF(nil, K_main, "ConfirmationKeys", 64); K_shared = HKDF(nil,
 * K_main, "SharedKey", 32). 0 or ERR_CRYPTO. The caller wipes *out. */
int sensei_commission_schedule(const struct sensei_secure_crypto *c, const uint8_t *tt, size_t tt_len,
                               const uint8_t share_p[SENSEI_COMMISSION_POINT_SIZE],
                               const uint8_t share_v[SENSEI_COMMISSION_POINT_SIZE],
                               struct sensei_commission_schedule *out);

/** okm = HKDF(ikm = K_shared, salt = "SENSEI-COMMISSION-v1", info = "channel" || node id (8 B BE) || nonce_G ||
 * nonce_N, L = 36): k_c2n = okm[0:16], k_n2c = okm[16:32], sid = u32 LE okm[32:36]. ERR_CRYPTO if sid is 0. */
int sensei_commission_channel_keys(const struct sensei_secure_crypto *c, const uint8_t k_shared[32], uint64_t node_id,
                                   const uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE],
                                   const uint8_t nonce_n[SENSEI_COMMISSION_NONCE_SIZE], uint8_t k_c2n[16],
                                   uint8_t k_n2c[16], uint32_t *sid);

/** The context: "SENSEI-COMMISSION-v1" || suite || kdf || log2n || r || p || salt || nonce_G || nonce_N (73 B). */
void sensei_commission_context(const struct sensei_commission_params *p,
                               const uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE],
                               const uint8_t nonce_n[SENSEI_COMMISSION_NONCE_SIZE],
                               uint8_t out[SENSEI_COMMISSION_CONTEXT_SIZE]);

/** "SENSEI-node" || node id (8 B big endian), 19 B. */
void sensei_commission_id_verifier(uint64_t node_id, uint8_t out[SENSEI_COMMISSION_ID_SIZE]);

/** The input of the scrypt: len(pw) || pw || len(idProver) || idProver || len(idVerifier) || idVerifier, lengths 8 B
 * little endian (RFC 9383 section 3.2), pw = the 11 digits of the code. Writes 73 bytes to out and returns 73, or
 * ERR_FORMAT if code is not a valid code (12 digits, check digit) or cap is below 73. */
int sensei_commission_pwin(const char *code, uint64_t node_id, uint8_t *out, size_t cap);

/** w0 and w1 from the 80 B scrypt output: the two 40 B halves reduced mod n. ERR_FORMAT if either is 0 (the bench
 * takes another salt), ERR_CRYPTO if the backend has no scalar_reduce. */
int sensei_commission_derive_w(const struct sensei_secure_crypto *c, const uint8_t dk[SENSEI_COMMISSION_DK_SIZE],
                               uint8_t w0[SENSEI_COMMISSION_SCALAR_SIZE], uint8_t w1[SENSEI_COMMISSION_SCALAR_SIZE]);

/** SHA-256("SENSEI-NODE-FP-v1" || node id (8 B BE) || node public key (65 B))[0:16]. */
int sensei_commission_fingerprint(const struct sensei_secure_crypto *c, uint64_t node_id,
                                  const uint8_t node_pub[SENSEI_SECURE_PUB_SIZE],
                                  uint8_t out[SENSEI_COMMISSION_FP_SIZE]);

/** SHA-256("SENSEI-COLLECTOR-FP-v1" || collector public key (65 B))[0:8]. */
int sensei_commission_collector_fingerprint(const struct sensei_secure_crypto *c,
                                            const uint8_t backend_pub[SENSEI_SECURE_PUB_SIZE],
                                            uint8_t out[SENSEI_COMMISSION_COLLECTOR_FP_SIZE]);

/** The plaintext of the proof of possession: "SENSEI-COMMISSION-pop-v1" || node id (8 B BE) || nonce_G || nonce_N. The
 * node seals it as kind 3 under k_up through sensei_e2e_seal() (the normal counter path), after the key step. */
void sensei_commission_proof_plain(uint64_t node_id, const uint8_t nonce_g[SENSEI_COMMISSION_NONCE_SIZE],
                                   const uint8_t nonce_n[SENSEI_COMMISSION_NONCE_SIZE],
                                   uint8_t out[SENSEI_COMMISSION_POP_SIZE]);

/* ---- setup code, QR payload, label (the commissioner's side; the node never sees the code) ---- */

/** Verhoeff check digit (0 ... 9) of n decimal digits (n may be 0), or -1 if a character is not a digit. */
int sensei_commission_verhoeff_digit(const char *digits, size_t n);

/** Read a code typed or scanned: 12 digits, dashes and blanks allowed anywhere. Writes the 12 digits and the end of
 * string to out (13 B). ERR_FORMAT for another length, a letter, or a wrong check digit. */
int sensei_commission_code_parse(const char *text, char out[SENSEI_COMMISSION_CODE_SIZE + 1]);

/** "1234-5678-9012" (14 characters and the end of string) from the 12 digits. */
void sensei_commission_code_format(const char code[SENSEI_COMMISSION_CODE_SIZE], char out[15]);

/** `SN1:<node id 16 hex>:<code 12 digits>:<fingerprint 32 hex>`, upper case, 66 characters and the end of string. The
 * code must be valid, the node id not 0 (ERR_FORMAT). */
int sensei_commission_qr_format(uint64_t node_id, const char *code, const uint8_t fp[SENSEI_COMMISSION_FP_SIZE],
                                char out[SENSEI_COMMISSION_QR_SIZE + 1]);

/** Inverse of qr_format(), strict (upper case hex, valid code, node id not 0; blanks around the text are ignored).
 * ERR_FORMAT otherwise. code receives 12 digits and the end of string. */
int sensei_commission_qr_parse(const char *text, uint64_t *node_id, char code[SENSEI_COMMISSION_CODE_SIZE + 1],
                               uint8_t fp[SENSEI_COMMISSION_FP_SIZE]);

/** The three lines of the label: `SENSEI 00A1B2C3 D4E5F607` (25 B with the end of string), `Setup code 1234-5678-9012`
 * (26 B) and `Key a1b2 c3d4 e5f6 0718` (24 B, the first 8 bytes of the fingerprint). */
void sensei_commission_label_lines(uint64_t node_id, const char code[SENSEI_COMMISSION_CODE_SIZE],
                                   const uint8_t fp[SENSEI_COMMISSION_FP_SIZE], char line_id[25], char line_code[26],
                                   char line_key[24]);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_COMMISSION_H_ */
