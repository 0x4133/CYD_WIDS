// contacts.h — persisted nickname↔MAC directory, used by ESP-NOW messaging.
#pragma once
#include <Arduino.h>

#define MAX_CONTACTS    32
#define CONTACT_NAME_MAX 16

struct Contact {
  uint8_t  mac[6];
  char     name[CONTACT_NAME_MAX + 1];
  uint32_t lastSeenMs;
  uint8_t  pubkey[32];   // X25519 public key of the peer, zero-filled if !hasKey
  bool     hasKey;
  // Cached AES-128 key = HKDF-SHA256(ECDH(ourPriv, theirPub), "cyd-chat-v1…").
  // Precomputed at key-set time so the per-packet seal/open path never does
  // ECDH (mbedtls_ecp_group_load + MPI ops) — that was the main heap-
  // fragmentation source in the chat path.
  uint8_t  aesKey[16];
  bool     hasAesKey;
};

void contactsBegin();              // load /contacts.csv if present
int  contactsCount();
const Contact* contactsGet(int idx);
int  contactsFindByMac(const uint8_t mac[6]);
bool contactsAdd(const uint8_t mac[6], const char* name);
bool contactsRename(int idx, const char* name);
bool contactsRemove(int idx);
void contactsTouch(const uint8_t mac[6]);   // bump lastSeenMs if the MAC is known
bool contactsSetPubkey(const uint8_t mac[6], const uint8_t pub[32]);  // creates an unnamed contact if MAC unknown
void contactsSave();                        // persist to /contacts.csv
