# SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0
#
# Authors:
# - Philip Wiese <wiesep@iis.ee.ethz.ch>
"""Keys at rest: sealed settings records of the node and the sealed key store file of the backend."""

from __future__ import annotations

import base64
import json
import os
import stat
import subprocess
import sys

import pytest
from sensei_sdk import secure, vectors
from sensei_sdk.secure import KIND_DOWNLINK, KIND_UPLINK, KeyStore, NodeSession, ReplayError, SecureError

NODE_ID = 0x0123456789ABCDEF
KEY16 = bytes(range(16))

# ---- sealed settings records (node) ----


def test_record_layout_and_roundtrip():
    priv = bytes(range(1, 33))
    rec = secure.seal_record(KEY16, "e2e_privx", priv)
    assert len(rec) == 32 + secure.SEAL_OVERHEAD == 61
    assert rec[0] == 1  # version
    assert secure.open_record(KEY16, "e2e_privx", rec) == priv
    assert secure.seal_record(KEY16, "e2e_privx", priv) != rec  # a new nonce every time
    assert secure.open_record(KEY16, "e2e_rat", secure.seal_record(KEY16, "e2e_rat", b"")) == b""


def test_record_vectors_are_the_ones_of_the_c_header():
    """Vector 7: the generator and the committed header agree (the C test checks the C side against the header)."""
    for name, nonce, plain in vectors.seal_cases():
        rec = secure.seal_record(vectors.SEAL_KEY, name, plain, nonce)
        assert rec[1:13] == nonce and secure.open_record(vectors.SEAL_KEY, name, rec) == plain
    header = (vectors.SDK_ROOT / vectors.SECURE_HEADER).read_text()
    name, nonce, plain = vectors.seal_cases()[0]
    first = secure.seal_record(vectors.SEAL_KEY, name, plain, nonce)
    assert ", ".join(f"0x{b:02X}" for b in first[:16]) in header


def test_record_is_bound_to_its_name_key_and_bytes():
    rec = secure.seal_record(KEY16, "e2e_privx", b"p" * 32)
    for name in ("e2e_rat", "e2e_priv", "e2e_privX"):
        with pytest.raises(SecureError):
            secure.open_record(KEY16, name, rec)
    with pytest.raises(SecureError):
        secure.open_record(bytes(16), "e2e_privx", rec)
    for i in range(len(rec)):
        bad = bytearray(rec)
        bad[i] ^= 1
        with pytest.raises(SecureError):
            secure.open_record(KEY16, "e2e_privx", bytes(bad))
    for bad in (rec[:-1], rec + b"\0", rec[:28], b""):
        with pytest.raises(SecureError):
            secure.open_record(KEY16, "e2e_privx", bad)
    with pytest.raises(SecureError, match = "name"):
        secure.seal_record(KEY16, "", b"x")
    with pytest.raises(SecureError, match = "name"):
        secure.seal_record(KEY16, "x" * 33, b"x")
    with pytest.raises(SecureError, match = "16"):
        secure.seal_record(bytes(32), "e2e_privx", b"x")
    with pytest.raises(SecureError, match = "12"):
        secure.seal_record(KEY16, "e2e_privx", b"x", nonce = bytes(8))


# ---- sealed key store file (backend) ----


@pytest.fixture
def pem(tmp_path):
    p = tmp_path / "backend.pem"
    secure.backend_keygen(p)
    return p


@pytest.fixture
def key_file(tmp_path):
    p = tmp_path / "secrets" / "keystore.key"
    secure.new_key_file(p)
    return p


def sealed_store(tmp_path, pem, key_file, name = "keys.json", **kw) -> KeyStore:
    return KeyStore.load(tmp_path / "store" / name, private_pem = pem, key_file = key_file, **kw)


def add_node(store: KeyStore) -> NodeSession:
    priv, pub = secure.generate_keypair()
    store.add_node(NODE_ID, pub)
    return NodeSession(priv, store.backend_public_key())


def outer(path) -> dict:
    return json.loads(path.read_text())


def test_key_file_is_32_raw_bytes_mode_0400(tmp_path, key_file):
    assert key_file.stat().st_size == 32 and stat.S_IMODE(key_file.stat().st_mode) == 0o400
    assert secure.load_key_file(key_file) == key_file.read_bytes()
    with pytest.raises(FileExistsError):
        secure.new_key_file(key_file)  # never overwrites a key
    short = tmp_path / "short.key"
    short.write_bytes(b"x" * 31)
    short.chmod(0o600)
    with pytest.raises(SecureError, match = "32 raw bytes"):
        secure.load_key_file(short)
    with pytest.raises(SecureError, match = "cannot read"):
        secure.load_key_file(tmp_path / "none.key")


def test_a_sealed_store_holds_no_secret_in_the_clear(tmp_path, pem, key_file):
    store = sealed_store(tmp_path, pem, key_file)
    node = add_node(store)
    k_up = store._nodes[NODE_ID].keys.k_up
    store.save()
    path = store.path
    raw = path.read_bytes()
    assert stat.S_IMODE(path.stat().st_mode) == 0o600 and store.sealed
    o = outer(path)
    assert o["format"] == "sensei-keystore-sealed" and o["version"] == 1
    assert set(o) == {"format", "version", "key_id", "nonce", "ct"}
    key = key_file.read_bytes()
    assert o["key_id"] == secure._file_key_id(key).hex() and len(o["nonce"]) == 24
    for needle in (k_up.hex().encode(), b"k_up", b"node_pub", store.backend_public_key().hex().encode(), b"senders"):
        assert needle not in raw
    # the file opens with the key file; counters and keys come back
    again = KeyStore.load(path, key_file = key_file)
    assert again.node_ids() == [NODE_ID] and again.key_id(NODE_ID) == store.key_id(NODE_ID)
    assert secure.open_sealed(node.seal(KIND_UPLINK, b"x"), again)[2] == b"x"
    # no leftover temp files, the lock stays a plain file next to it
    assert sorted(p.name for p in path.parent.iterdir()) == ["keys.json", "keys.json.lock"]


def test_every_save_uses_a_new_nonce(tmp_path, pem, key_file):
    store = sealed_store(tmp_path, pem, key_file)
    add_node(store)
    nonces = set()
    for i in range(5):
        secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store)  # saves at once
        nonces.add(outer(store.path)["nonce"])
        assert len(nonces) == i + 1
    cts = {outer(store.path)["ct"]}
    store.save()
    assert outer(store.path)["ct"] not in cts  # same content, other nonce, other ciphertext


def test_sealed_store_needs_its_key_and_names_a_wrong_one(tmp_path, pem, key_file):
    store = sealed_store(tmp_path, pem, key_file)
    add_node(store)
    path = store.path
    with pytest.raises(SecureError, match = "sealed: give the key file"):
        KeyStore.load(path)
    other = tmp_path / "other.key"
    secure.new_key_file(other)
    with pytest.raises(SecureError, match = "does not belong to key store"):
        KeyStore.load(path, key_file = other)
    # a wrong key never writes: the file is as it was
    before = path.read_bytes()
    with pytest.raises(SecureError):
        KeyStore.load(path, key_file = other).save()
    assert path.read_bytes() == before


def test_changed_file_is_refused(tmp_path, pem, key_file):
    store = sealed_store(tmp_path, pem, key_file)
    add_node(store)
    o = outer(store.path)
    ct = bytearray(base64.b64decode(o["ct"]))
    ct[10] ^= 1
    tampered = dict(o, ct = base64.b64encode(bytes(ct)).decode())
    store.path.write_text(json.dumps(tampered))
    with pytest.raises(SecureError, match = "failed authentication"):
        KeyStore.load(store.path, key_file = key_file)
    # the nonce and the key id are covered as well (the key id through the associated data and the key check)
    for field, value in (("nonce", "00" * 12), ("key_id", "00" * 8)):
        store.path.write_text(json.dumps(dict(o, **{field: value})))
        with pytest.raises(SecureError):
            KeyStore.load(store.path, key_file = key_file)
    store.path.write_text(json.dumps(dict(o, version = 2)))
    with pytest.raises(SecureError, match = "unsupported sealed key store version"):
        KeyStore.load(store.path, key_file = key_file)


def test_plain_store_is_read_with_a_key_and_sealed_at_the_next_save(tmp_path, pem, key_file):
    plain = KeyStore.load(tmp_path / "store" / "keys.json", private_pem = pem)
    node = add_node(plain)
    assert "k_up" in plain.path.read_text() and not plain.sealed
    store = KeyStore.load(plain.path, private_pem = pem, key_file = key_file)
    assert store.file_was_plain and store.sealed and store.node_ids() == [NODE_ID]
    secure.open_sealed(node.seal(KIND_UPLINK, b"x"), store)
    store.save()
    assert outer(store.path)["format"] == "sensei-keystore-sealed"
    assert not KeyStore.load(store.path, key_file = key_file).file_was_plain


def test_a_plain_store_without_a_key_warns_once(tmp_path, pem, caplog):
    plain = KeyStore.load(tmp_path / "keys.json", private_pem = pem)
    add_node(plain)
    caplog.clear()
    store = KeyStore.load(plain.path)
    store.save()
    store.save()
    assert store.file_was_plain and not store.sealed
    assert len([r for r in caplog.records if "not sealed" in r.message]) == 1


def test_require_sealed(tmp_path, pem, key_file):
    plain = KeyStore.load(tmp_path / "keys.json", private_pem = pem)
    add_node(plain)
    with pytest.raises(SecureError, match = "not sealed but SENSEI_KEYSTORE_REQUIRE_SEALED=1"):
        KeyStore.load(plain.path, key_file = key_file, require_sealed = True)
    with pytest.raises(SecureError, match = "needs a key file"):
        KeyStore.load(plain.path, require_sealed = True)
    # an empty store is made sealed
    store = KeyStore.load(tmp_path / "new.json", private_pem = pem, key_file = key_file, require_sealed = True)
    add_node(store)
    assert outer(store.path)["format"] == "sensei-keystore-sealed"
    assert KeyStore.load(store.path, key_file = key_file, require_sealed = True).node_ids() == [NODE_ID]


def test_a_key_file_that_is_set_but_missing_is_an_error(tmp_path, pem):
    with pytest.raises(SecureError, match = "cannot read"):
        KeyStore.load(tmp_path / "keys.json", private_pem = pem, key_file = tmp_path / "gone.key")


def test_environment_variables(tmp_path, pem, key_file, monkeypatch):
    monkeypatch.setenv("SENSEI_KEYSTORE_KEY_FILE", str(key_file))
    monkeypatch.setenv("SENSEI_KEYSTORE_REQUIRE_SEALED", "1")
    store = KeyStore.load(tmp_path / "keys.json", private_pem = pem)
    add_node(store)
    assert store.sealed and outer(store.path)["format"] == "sensei-keystore-sealed"
    monkeypatch.delenv("SENSEI_KEYSTORE_KEY_FILE")
    with pytest.raises(SecureError, match = "needs a key file"):
        KeyStore.load(store.path)


def test_counters_survive_a_sealed_restart(tmp_path, pem, key_file):
    store = sealed_store(tmp_path, pem, key_file)
    node = add_node(store)
    for _ in range(3):
        secure.open_sealed(node.seal(KIND_UPLINK, b"x"), store)
    secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store)
    store.save()
    again = KeyStore.load(store.path, key_file = key_file)
    with pytest.raises(ReplayError):
        secure.open_sealed(node.seal(KIND_UPLINK, b"old", counter = 2), again)
    assert again.down_counter(NODE_ID) == 1
    assert secure.parse_header(secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"y", again)).counter == 2


_WORKER = """
import sys
from sensei_sdk import secure
store = secure.KeyStore.load(sys.argv[1])  # key file from SENSEI_KEYSTORE_KEY_FILE
for _ in range(int(sys.argv[2])):
    env = secure.seal_downlink(int(sys.argv[3]), secure.KIND_DOWNLINK, b"x", store)
    print(secure.parse_header(env).counter)
"""


def test_processes_share_a_sealed_file_and_never_reuse_a_counter(tmp_path, pem, key_file):
    store = sealed_store(tmp_path, pem, key_file)
    add_node(store)
    env = dict(os.environ, SENSEI_KEYSTORE_KEY_FILE = str(key_file), SENSEI_KEYSTORE_REQUIRE_SEALED = "1")
    procs = [
        subprocess.Popen([sys.executable, "-c", _WORKER,
                          str(store.path), "10", str(NODE_ID)],
                         stdout = subprocess.PIPE,
                         text = True,
                         env = env) for _ in range(4)
    ]
    counters = []
    for p in procs:
        out, _err = p.communicate(timeout = 120)
        assert p.returncode == 0
        counters += [int(x) for x in out.split()]
    assert sorted(counters) == list(range(1, 41))
    assert KeyStore.load(store.path, key_file = key_file).down_counter(NODE_ID) == 40


def test_a_message_is_not_lost_when_the_key_file_changes_under_a_running_store(tmp_path, pem, key_file, caplog):
    """rotate-file-key (or seal-file) on the file while the process runs: the autosave of a replay counter fails with a
    SecureError. The uplink that moved the counter is still returned (the counter is marked in RAM), the failure is
    logged and retried later. A downlink counter (must be saved at once) still raises."""
    store = sealed_store(tmp_path, pem, key_file)
    store.autosave_s = 0  # every uplink tries to save
    node = add_node(store)
    secure.open_sealed(node.seal(KIND_UPLINK, b"one"), store)
    other = tmp_path / "secrets" / "other.key"
    secure.rotate_file_key(store.path, key_file, other, create_key = True)  # the running store keeps the old key

    with caplog.at_level("WARNING"):
        kind_plain = secure.open_sealed(node.seal(KIND_UPLINK, b"two"), store)
    assert kind_plain[2] == b"two"
    assert "not saved" in caplog.text
    assert secure.open_sealed(node.seal(KIND_UPLINK, b"three"), store)[2] == b"three"  # and the next one
    with pytest.raises(ReplayError):  # the counter is used: a repeat is a replay
        secure.open_sealed(node.seal(KIND_UPLINK, b"x", counter = 2), store)
    with pytest.raises(SecureError, match = "does not belong"):
        secure.seal_downlink(NODE_ID, KIND_DOWNLINK, b"x", store)
    # a restart with the new key file has the node and the counters of what was saved
    assert KeyStore.load(store.path, key_file = other).node_ids() == [NODE_ID]


def test_a_plain_store_that_gets_sealed_under_a_running_process_keeps_its_uplinks(tmp_path, pem, key_file):
    store = KeyStore.load(tmp_path / "keys.json", private_pem = pem)
    store.autosave_s = 0
    node = add_node(store)
    secure.seal_file(store.path, key_file)  # the running store has no key
    assert secure.open_sealed(node.seal(KIND_UPLINK, b"x"), store)[2] == b"x"  # the save fails, the message is fine


def test_the_key_file_is_synced_with_its_folder(tmp_path, monkeypatch):
    synced = []
    real_fsync = os.fsync
    monkeypatch.setattr(os, "fsync", lambda fd: (synced.append(os.fstat(fd).st_mode), real_fsync(fd))[1])
    secure.new_key_file(tmp_path / "keys" / "a.key")
    assert any(stat.S_ISREG(m) for m in synced), "the key file is synced"
    assert any(stat.S_ISDIR(m) for m in synced), "its folder is synced"


def test_a_key_file_that_cannot_be_written_is_removed(tmp_path, monkeypatch):

    def broken(fd):
        raise OSError("disk full")

    monkeypatch.setattr(os, "fsync", broken)
    p = tmp_path / "a.key"
    with pytest.raises(OSError, match = "disk full"):
        secure.new_key_file(p)
    assert not p.exists()  # no empty key file that blocks the next try


@pytest.mark.skipif(os.name == "nt", reason = "modes")
def test_a_key_file_that_others_can_read_is_refused(tmp_path, pem):
    p = tmp_path / "k.key"
    secure.new_key_file(p)
    secure.load_key_file(p)
    p.chmod(0o440)
    with pytest.raises(SecureError, match = r"can be read by other users \(mode 0440\).*chmod 400"):
        secure.load_key_file(p)
    with pytest.raises(SecureError, match = "other users"):
        KeyStore.load(tmp_path / "keys.json", private_pem = pem, key_file = p)
    assert secure.load_key_file(p, check_mode = False) == p.read_bytes()
    p.chmod(0o604)
    with pytest.raises(SecureError, match = "0604"):
        secure.load_key_file(p)
    p.chmod(0o600)
    secure.load_key_file(p)
    with pytest.raises(SecureError, match = "not a file"):
        secure.check_key_file_mode(tmp_path)
    with pytest.raises(SecureError, match = "cannot read"):
        secure.load_key_file(tmp_path)


# ---- command line ----


def run_cli(*args) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-m", "sensei_sdk.secure", *args],
                          capture_output = True,
                          text = True,
                          timeout = 60,
                          env = {
                              k: v for k, v in os.environ.items() if not k.startswith("SENSEI_KEYSTORE")
                          })


def test_cli_seal_unseal_rotate(tmp_path, pem):
    plain = KeyStore.load(tmp_path / "keys.json", private_pem = pem)
    node = add_node(plain)
    secure.open_sealed(node.seal(KIND_UPLINK, b"x"), plain)
    plain.save()
    path, k1, k2 = tmp_path / "keys.json", tmp_path / "k1.key", tmp_path / "k2.key"
    before = json.loads(path.read_text())

    r = run_cli("seal-file", str(path))
    assert r.returncode == 2 and "--key-file" in r.stderr  # a key file is needed
    r = run_cli("seal-file", str(path), "--key-file", str(k1))
    assert r.returncode == 1 and "cannot read" in r.stderr  # a missing key file is not made by itself
    r = run_cli("seal-file", str(path), "--key-file", str(k1), "--create-key")
    assert r.returncode == 0, r.stderr
    assert outer(path)["format"] == "sensei-keystore-sealed" and stat.S_IMODE(k1.stat().st_mode) == 0o400
    assert KeyStore.load(path, key_file = k1).node_ids() == [NODE_ID]
    r = run_cli("seal-file", str(path), "--key-file", str(k1))
    assert r.returncode == 1 and "sealed already" in r.stderr

    r = run_cli("rotate-file-key", str(path), "--key-file", str(k1), "--new-key-file", str(k1))
    assert r.returncode == 1 and "holds the old key" in r.stderr
    r = run_cli("rotate-file-key", str(path), "--key-file", str(k1), "--new-key-file", str(k2), "--create-key")
    assert r.returncode == 0, r.stderr
    with pytest.raises(SecureError, match = "does not belong"):
        KeyStore.load(path, key_file = k1)
    rotated = KeyStore.load(path, key_file = k2)
    assert rotated.key_id(NODE_ID) == plain.key_id(NODE_ID)

    r = run_cli("unseal-file", str(path), "--key-file", str(k1))
    assert r.returncode == 1 and "does not belong" in r.stderr
    r = run_cli("unseal-file", str(path), "--key-file", str(k2))
    assert r.returncode == 0, r.stderr
    assert json.loads(path.read_text()) == before  # byte for byte the same content as before sealing
    r = run_cli("unseal-file", str(path), "--key-file", str(k2))
    assert r.returncode == 1 and "not sealed" in r.stderr
    r = run_cli("rotate-file-key", str(path), "--key-file", str(k2), "--new-key-file", str(k1))
    assert r.returncode == 1 and "not sealed" in r.stderr


def test_cli_new_key_file_and_key_from_the_environment(tmp_path, pem):
    store = KeyStore.load(tmp_path / "keys.json", private_pem = pem)
    add_node(store)
    k = tmp_path / "k.key"
    assert run_cli("new-key-file", str(k)).returncode == 0 and k.stat().st_size == 32
    assert run_cli("new-key-file", str(k)).returncode == 1  # exists
    r = subprocess.run([sys.executable, "-m", "sensei_sdk.secure", "seal-file",
                        str(store.path)],
                       capture_output = True,
                       text = True,
                       timeout = 60,
                       env = dict(os.environ, SENSEI_KEYSTORE_KEY_FILE = str(k)))
    assert r.returncode == 0, r.stderr
    assert outer(store.path)["format"] == "sensei-keystore-sealed"
