// file_xfer.h — ESP-NOW unicast file transfer with CRC32 integrity.
// V1 protocol: sender fires chunks at fixed spacing; receiver writes to a
// .partial file; on all-received + CRC match, file is renamed into place.
#pragma once
#include <Arduino.h>
#include "contacts.h"

#define XFER_CHUNK_BYTES  200          // data bytes per FILE_CHUNK frame
#define XFER_NAME_MAX     32

// Frame tags used on the wire (defined once here, consumed by espnow_chat).
#define TAG_FILE_OFFER    0x10
#define TAG_FILE_ACCEPT   0x11
#define TAG_FILE_DECLINE  0x12
#define TAG_FILE_CHUNK    0x13
#define TAG_FILE_ACK      0x14   // legacy; unused in v2 async protocol
#define TAG_FILE_CANCEL   0x15
#define TAG_FILE_DONE     0x16
// v2 async retransmit: receiver → sender with [count:2][start:2][end:2]...
// where each [start,end) is a contiguous run of missing chunk seqs.
#define TAG_FILE_MISSING  0x17

enum XferState : uint8_t {
  XS_IDLE = 0,
  XS_OFFERED,     // receiver: OFFER arrived, awaiting user accept/decline
  XS_OFFERING,    // sender: sent OFFER, waiting for ACCEPT
  XS_RECEIVING,   // receiver: accepted, collecting chunks (any order)
  XS_SENDING,     // sender: initial pass — streaming every chunk once
  XS_REPAIR,      // sender: initial pass done, servicing MISSING retransmits
  XS_WAIT_DONE,   // sender: receiver reported complete, awaiting FILE_DONE
  XS_DONE,        // terminal ok
  XS_CANCELED,    // terminal abort
  XS_FAILED,      // terminal failure (timeout / CRC / io)
};

struct XferStatus {
  XferState state;
  bool      incoming;            // true => we're the receiver
  uint8_t   peerMac[6];
  char      peerName[CONTACT_NAME_MAX + 1];
  char      fileName[XFER_NAME_MAX + 1];
  uint32_t  fileSize;
  uint32_t  bytesDone;
  uint16_t  chunksDone;
  uint16_t  chunksTotal;
  uint32_t  startMs;
};

void fileXferBegin();

// Called by espnow_chat's onRecv when a FILE_* tagged frame arrives.
void fileXferOnFrame(const uint8_t srcMac[6], uint8_t tag,
                     const uint8_t* payload, int plen);

// Sender entry: read `path`, compute size+CRC, send FILE_OFFER to dest.
// dest must already be a known contact with hasKey == true.
bool fileXferStartSend(const uint8_t destMac[6], const char* path);

// Receiver modal actions.
bool fileXferOfferPending();
void fileXferAcceptIncoming();
void fileXferDeclineIncoming();

// Either side: user pressed CANCEL during active transfer.
void fileXferCancel();

// Snapshot for UI — returns false if state is idle.
bool fileXferStatus(XferStatus* out);

// Drive sender chunk pump + timeouts. Call from loop().
void fileXferTick();
