// espnow_chat.h
#pragma once
#include <Arduino.h>
#include "config.h"

struct ChatMsg {
  uint8_t  mac[6];                   // sender MAC; ours for outgoing
  bool     mine;
  uint32_t tMs;
  char     text[CHAT_MSG_MAX + 1];
};

bool chatBegin();                    // init esp_now, add broadcast peer
// Encrypted unicast send to the currently-selected target. Returns false if
// no target is set, the target has no pubkey, or encryption fails.
bool chatSend(const char* text);
bool chatSendHello();                // tiny presence beacon (no UI entry)

// Per-session target selector. The UI cycles through contacts with hasKey=true
// and calls chatSetTarget. A zero MAC clears the target.
void chatSetTarget(const uint8_t mac[6]);
bool chatHasTarget();
void chatGetTarget(uint8_t out[6]);
int  chatSnapshot(ChatMsg* out, int maxOut);  // newest first
uint32_t chatRecvCount();
int  chatPeerCount();                // distinct peers heard (chat OR hello)

// Snapshot of the peer table (MAC + last-seen ms). Used by the contacts
// screen to let the user promote heard peers into named contacts.
int  chatPeerList(uint8_t outMac[][6], uint32_t* outLastMs, int maxOut);

// Ensure `mac` is registered as an ESP-NOW unicast peer (idempotent).
bool chatEnsurePeer(const uint8_t mac[6]);

// Send our X25519 pubkey to `mac` (KEY_REQ). Peer replies with KEY_RESP
// which is handled internally and persisted into contacts. Returns false if
// crypto isn't ready or the unicast peer can't be added.
bool chatRequestKey(const uint8_t mac[6]);

// Auto-resend counters for the UI (ok/fail totals since boot).
uint32_t chatSendOk();
uint32_t chatSendFail();

// Print a [CHAT-DIAG] line with channel/ok/fail/pending/callback-age. Call
// periodically from loop() to surface messaging health without waiting for
// the UI. No-op-safe; never allocates.
void chatDiagPrint();
