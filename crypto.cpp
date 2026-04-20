// crypto.cpp — X25519 via mbedTLS (bundled with ESP-IDF).
// Key layout on SD: 64 bytes = [priv 32][pub 32], little-endian raw form.
#include "crypto.h"
#include "sd_safe.h"
#include <SD.h>
#include <esp_system.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

static uint8_t ourPriv[CRYPTO_KEY_LEN];
static uint8_t ourPub [CRYPTO_KEY_LEN];
static bool    ready = false;

static int espRng(void*, unsigned char* out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

static bool generateKeypair() {
  mbedtls_ecp_group grp;  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi       d;    mbedtls_mpi_init(&d);
  mbedtls_ecp_point Q;    mbedtls_ecp_point_init(&Q);

  bool ok = false;
  size_t olen = 0;
  int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
  if (rc == 0) rc = mbedtls_ecdh_gen_public(&grp, &d, &Q, espRng, nullptr);
  if (rc == 0) rc = mbedtls_mpi_write_binary_le(&d, ourPriv, CRYPTO_KEY_LEN);
  // For CURVE25519, write_binary with UNCOMPRESSED emits the u-coordinate
  // as 32 LE bytes (olen == 32). Passing nullptr for olen would fault.
  if (rc == 0) rc = mbedtls_ecp_point_write_binary(&grp, &Q,
                        MBEDTLS_ECP_PF_UNCOMPRESSED,
                        &olen, ourPub, CRYPTO_KEY_LEN);
  ok = (rc == 0 && olen == CRYPTO_KEY_LEN);

  mbedtls_ecp_point_free(&Q);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}

static bool loadFromSd() {
  File f = SD.open("/keypair.bin", FILE_READ);
  if (!f) return false;
  uint8_t buf[64];
  int n = f.read(buf, sizeof(buf));
  f.close();
  if (n != 64) return false;
  memcpy(ourPriv, buf,      CRYPTO_KEY_LEN);
  memcpy(ourPub,  buf + 32, CRYPTO_KEY_LEN);
  return true;
}

static bool saveToSd() {
  uint8_t buf[64];
  memcpy(buf,      ourPriv, CRYPTO_KEY_LEN);
  memcpy(buf + 32, ourPub,  CRYPTO_KEY_LEN);
  // Atomic write for robustness against reset mid-write.
  File f = SD.open("/keypair.bin.new", FILE_WRITE);
  if (!f) return false;
  size_t w = f.write(buf, sizeof(buf));
  f.flush(); f.close();
  if (w != sizeof(buf)) return false;
  if (SD.exists("/keypair.bin")) SD.remove("/keypair.bin");
  return SD.rename("/keypair.bin.new", "/keypair.bin");
}

bool cryptoBegin() {
  if (loadFromSd()) {
    ready = true;
    Serial.println("[CRYPTO] keypair loaded");
    return true;
  }
  Serial.println("[CRYPTO] generating new keypair");
  if (!generateKeypair()) { Serial.println("[CRYPTO] gen failed"); return false; }
  if (!saveToSd()) {
    Serial.println("[CRYPTO] save failed — keypair only in RAM");
  }
  ready = true;
  Serial.printf("[CRYPTO] pub %02X%02X%02X...%02X%02X%02X\n",
    ourPub[0], ourPub[1], ourPub[2],
    ourPub[29], ourPub[30], ourPub[31]);
  return true;
}

void cryptoGetOurPubkey(uint8_t out[CRYPTO_KEY_LEN]) {
  memcpy(out, ourPub, CRYPTO_KEY_LEN);
}

bool cryptoReady() { return ready; }

// HKDF-SHA256(shared, salt=empty, info="cyd-chat-v1 aes128gcm") → 16-byte key.
// TLS 1.3 uses HKDF for the same reason: whiten the raw DH output into a
// cryptographically-uniform key and bind it to a domain/purpose string.
static bool deriveAesKey(const uint8_t theirPub[CRYPTO_KEY_LEN], uint8_t key[16]) {
  uint8_t shared[CRYPTO_KEY_LEN];
  if (!cryptoDeriveShared(theirPub, shared)) return false;
  const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return false;
  static const char info[] = "cyd-chat-v1 aes128gcm";
  int rc = mbedtls_hkdf(md, nullptr, 0,
                        shared, CRYPTO_KEY_LEN,
                        (const unsigned char*)info, sizeof(info) - 1,
                        key, 16);
  // Don't leave raw DH output on the stack.
  for (int i = 0; i < CRYPTO_KEY_LEN; i++) shared[i] = 0;
  return rc == 0;
}

bool cryptoSealWithKey(const uint8_t key[CRYPTO_AES_KEY_LEN],
                       const uint8_t* plain, int plainLen,
                       uint8_t* out, int outCap, int* outLen) {
  if (!ready || plainLen < 0) return false;
  if (outCap < plainLen + CRYPTO_AEAD_OVERHEAD) return false;

  uint8_t* nonce = out;           // 12 bytes
  uint8_t* ct    = out + 12;      // plainLen bytes
  uint8_t* tag   = ct  + plainLen; // 16 bytes
  esp_fill_random(nonce, 12);

  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, plainLen,
                                   nonce, 12, nullptr, 0,
                                   plain, ct, 16, tag);
  }
  mbedtls_gcm_free(&g);
  if (rc != 0) return false;
  *outLen = plainLen + CRYPTO_AEAD_OVERHEAD;
  return true;
}

bool cryptoOpenWithKey(const uint8_t key[CRYPTO_AES_KEY_LEN],
                       const uint8_t* in, int inLen,
                       uint8_t* plain, int plainCap, int* plainLen) {
  if (!ready || inLen < CRYPTO_AEAD_OVERHEAD) return false;
  int ctLen = inLen - CRYPTO_AEAD_OVERHEAD;
  if (plainCap < ctLen) return false;

  const uint8_t* nonce = in;
  const uint8_t* ct    = in + 12;
  const uint8_t* tag   = ct + ctLen;

  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(&g, ctLen,
                                  nonce, 12, nullptr, 0,
                                  tag, 16, ct, plain);
  }
  mbedtls_gcm_free(&g);
  if (rc != 0) return false;  // tag mismatch → tamper or wrong key
  *plainLen = ctLen;
  return true;
}

bool cryptoDeriveAesForPeer(const uint8_t theirPub[CRYPTO_KEY_LEN],
                            uint8_t outKey[CRYPTO_AES_KEY_LEN]) {
  return deriveAesKey(theirPub, outKey);
}

// Legacy wrappers: seal/open given a raw peer pubkey. These run a full ECDH
// per call; prefer the *WithKey variants + a cached AES key for per-packet
// paths. Kept so non-hot callers (first-time paths, debug) still compile.
bool cryptoSealForPeer(const uint8_t theirPub[CRYPTO_KEY_LEN],
                       const uint8_t* plain, int plainLen,
                       uint8_t* out, int outCap, int* outLen) {
  uint8_t key[CRYPTO_AES_KEY_LEN];
  if (!deriveAesKey(theirPub, key)) return false;
  bool ok = cryptoSealWithKey(key, plain, plainLen, out, outCap, outLen);
  for (int i = 0; i < CRYPTO_AES_KEY_LEN; i++) key[i] = 0;
  return ok;
}

bool cryptoOpenFromPeer(const uint8_t theirPub[CRYPTO_KEY_LEN],
                        const uint8_t* in, int inLen,
                        uint8_t* plain, int plainCap, int* plainLen) {
  uint8_t key[CRYPTO_AES_KEY_LEN];
  if (!deriveAesKey(theirPub, key)) return false;
  bool ok = cryptoOpenWithKey(key, in, inLen, plain, plainCap, plainLen);
  for (int i = 0; i < CRYPTO_AES_KEY_LEN; i++) key[i] = 0;
  return ok;
}

bool cryptoDeriveShared(const uint8_t theirPub[CRYPTO_KEY_LEN],
                        uint8_t outShared[CRYPTO_KEY_LEN]) {
  if (!ready) return false;
  mbedtls_ecp_group grp;  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi       d;    mbedtls_mpi_init(&d);
  mbedtls_mpi       z;    mbedtls_mpi_init(&z);
  mbedtls_ecp_point Qp;   mbedtls_ecp_point_init(&Qp);

  bool ok = false;
  int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
  if (rc == 0) rc = mbedtls_mpi_read_binary_le(&d, ourPriv, CRYPTO_KEY_LEN);
  if (rc == 0) rc = mbedtls_ecp_point_read_binary(&grp, &Qp, theirPub, CRYPTO_KEY_LEN);
  if (rc == 0) rc = mbedtls_ecdh_compute_shared(&grp, &z, &Qp, &d, espRng, nullptr);
  if (rc == 0) rc = mbedtls_mpi_write_binary_le(&z, outShared, CRYPTO_KEY_LEN);
  ok = (rc == 0);

  mbedtls_ecp_point_free(&Qp);
  mbedtls_mpi_free(&z);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_group_free(&grp);
  return ok;
}
