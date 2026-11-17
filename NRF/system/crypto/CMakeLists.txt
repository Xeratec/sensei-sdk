# Copyright (c) 2025 ETH Zurich and University of Bologna
# SPDX-License-Identifier: Apache-2.0

target_sources(app PRIVATE
    sensei_crypto.c
)

target_sources_ifdef(CONFIG_SENSEI_CRYPTO_AEAD app PRIVATE sensei_crypto_aead.c)
target_sources_ifdef(CONFIG_SENSEI_CRYPTO_ECDH app PRIVATE sensei_crypto_ecdh.c)
target_sources_ifdef(CONFIG_SENSEI_CRYPTO_ECDSA app PRIVATE sensei_crypto_ecdsa.c)

if(CONFIG_SENSEI_CRYPTO_HMAC OR CONFIG_SENSEI_CRYPTO_HKDF)
    target_sources(app PRIVATE sensei_crypto_kdf.c)
endif()
