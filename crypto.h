// crypto.h — X25519 keypair + shared-secret derivation for per-contact E2E.
#pragma once
#include <Arduino.h>

#define CRYPTO_KEY_LEN 32

// Loads the device keypair from /keypair.bin, or generates + saves one on
// first boot. Must be called before any Get/Derive.
bool cryptoBegin();

// Our long-term public key (32 bytes, raw Curve25519 u-coordinate).
void cryptoGetOurPubkey(uint8_t out[CRYPTO_KEY_LEN]);

// ECDH: derive 32-byte shared secret from our private + peer's public.
// Output is raw X25519 shared; caller may hash it before use as a symmetric key.
bool cryptoDeriveShared(const uint8_t theirPub[CRYPTO_KEY_LEN],
                        uint8_t outShared[CRYPTO_KEY_LEN]);

// True iff cryptoBegin() succeeded.
bool cryptoReady();

// AEAD (AES-128-GCM) framed as [nonce:12][ciphertext:plainLen][tag:16].
// Key is HKDF-SHA256(shared, info="cyd-chat-v1 aes128gcm"). TLS-style:
// ephemeral X25519 key agreement + HKDF + AEAD.
#define CRYPTO_AEAD_OVERHEAD 28   // 12-byte nonce + 16-byte GCM tag

// Encrypts `plain` (plainLen bytes) for the peer whose long-term X25519 pubkey
// is `theirPub`. Writes [nonce:12][ct][tag:16] into `out`; sets *outLen.
// outCap must be >= plainLen + CRYPTO_AEAD_OVERHEAD.
bool cryptoSealForPeer(const uint8_t theirPub[CRYPTO_KEY_LEN],
                       const uint8_t* plain, int plainLen,
                       uint8_t* out, int outCap, int* outLen);

// Inverse of cryptoSealForPeer. Verifies the GCM tag — returns false on any
// tamper / wrong-key / truncation. plainCap must be >= inLen - CRYPTO_AEAD_OVERHEAD.
bool cryptoOpenFromPeer(const uint8_t theirPub[CRYPTO_KEY_LEN],
                        const uint8_t* in, int inLen,
                        uint8_t* plain, int plainCap, int* plainLen);

// Pre-derived-key variants. The seal/open-ForPeer/FromPeer functions run a
// full ECDH (mbedtls_ecp_group_load(CURVE25519) + MPI ops) per frame, which
// under heap pressure fragments free memory and triggers AEAD failures. The
// contact table derives the 16-byte AES key once at key-set time and uses
// these variants on the per-packet path.
#define CRYPTO_AES_KEY_LEN 16

bool cryptoDeriveAesForPeer(const uint8_t theirPub[CRYPTO_KEY_LEN],
                            uint8_t outKey[CRYPTO_AES_KEY_LEN]);

bool cryptoSealWithKey(const uint8_t key[CRYPTO_AES_KEY_LEN],
                       const uint8_t* plain, int plainLen,
                       uint8_t* out, int outCap, int* outLen);

bool cryptoOpenWithKey(const uint8_t key[CRYPTO_AES_KEY_LEN],
                       const uint8_t* in, int inLen,
                       uint8_t* plain, int plainCap, int* plainLen);
