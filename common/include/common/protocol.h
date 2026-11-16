// Node to master wire protocol (draft v0.1)
//
// Every message is a 12-byte header followed by a payload of payload_len
// bytes. All fields are big-endian (network byte order) and are packed one at
// a time by the functions below. Raw structs are never sent.
//
// Values marked PROPOSED have not been agreed on yet.
#ifndef COMMON_PROTOCOL_H
#define COMMON_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define PROTO_VERSION       1
#define PROTO_MAGIC         0x534C  // PROPOSED
#define PROTO_PORT          35100   // PROPOSED
#define PROTO_MAX_PAYLOAD   1024    // PROPOSED
#define PROTO_MASTER_ID     0       // node_id used by the master

// Timing (milliseconds)
#define PROTO_HEARTBEAT_MS  1000    // PROPOSED: node sends HEARTBEAT after this long idle
#define PROTO_TIMEOUT_MS    3000    // PROPOSED: master drops a node after this long silent
#define PROTO_RECONNECT_MS  1000    // PROPOSED: node retries the connection this often

// Packed sizes in bytes (payload sizes exclude the header)
#define PROTO_HEADER_LEN    12
#define PROTO_HELLO_LEN     8
#define PROTO_HELLO_ACK_LEN 4
#define PROTO_BYE_LEN       4
#define PROTO_EVENT_LEN     40
#define PROTO_HEARTBEAT_LEN 20

// Largest possible message, for sizing receive buffers
#define PROTO_MAX_MSG_LEN   (PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD)

enum proto_msg_type {
    PROTO_MSG_HELLO     = 0x01, // node -> master
    PROTO_MSG_HELLO_ACK = 0x02, // master -> node
    PROTO_MSG_BYE       = 0x03, // either direction
    PROTO_MSG_EVENT     = 0x10, // node -> master
    PROTO_MSG_HEARTBEAT = 0x20, // node -> master
    // 0x30 - 0x3F reserved (possible ping/pong for latency measurement)
    // 0x40 - 0x4F reserved (master -> node control commands)
};

enum proto_hello_status {
    PROTO_HELLO_OK         = 0,
    PROTO_HELLO_UNKNOWN_ID = 1,
    PROTO_HELLO_DUPLICATE  = 2,
    PROTO_HELLO_VERSION    = 3,
};

enum proto_bye_reason {
    PROTO_BYE_SHUTDOWN       = 0,
    PROTO_BYE_PROTOCOL_ERROR = 1,
    PROTO_BYE_REPLACED       = 2,
};

// EVENT flags (PROPOSED, final bits are up to DSP)
#define PROTO_EVENT_PHI_VALID    (1u << 0)
#define PROTO_EVENT_THETA_MIRROR (1u << 1) // theta has a front/back mirror solution

// Return codes for the unpack functions
enum proto_result {
    PROTO_OK          =  0,
    PROTO_ERR_MAGIC   = -1, // stream is out of sync, close the connection
    PROTO_ERR_VERSION = -2, // header is still filled in so the master can reply
    PROTO_ERR_LEN     = -3, // payload too long, or wrong size for its type
};

// Header (12 bytes)
//   0  u16  magic
//   2  u8   version
//   3  u8   msg_type
//   4  u16  payload_len
//   6  u16  node_id      (0 = master)
//   8  u32  seq          per-sender counter for logs and debugging
struct proto_header {
    uint16_t magic;
    uint8_t  version;
    uint8_t  msg_type;
    uint16_t payload_len;
    uint16_t node_id;
    uint32_t seq;
};

// HELLO (8 bytes)
//   0  u8   mic_count
//   1  3    reserved
//   4  u32  build_id     e.g. part of the git hash
struct proto_hello {
    uint8_t  mic_count;
    uint32_t build_id;
};

// HELLO_ACK (4 bytes)
//   0  u8   status       enum proto_hello_status
//   1  1    reserved
//   2  u16  heartbeat_ms
struct proto_hello_ack {
    uint8_t  status;
    uint16_t heartbeat_ms;
};

// BYE (4 bytes)
//   0  u8   reason       enum proto_bye_reason
//   1  3    reserved
struct proto_bye {
    uint8_t reason;
};

// EVENT (40 bytes)
//   0  u64  t_event_ns       event time (meaning is up to DSP)
//   8  u64  t_send_ns        CLOCK_REALTIME when sent, for latency measurement
//  16  f32  theta            units and frame are up to DSP
//  20  f32  phi              only valid if PROTO_EVENT_PHI_VALID is set
//  24  f32  confidence       PROPOSED: 0.0 to 1.0
//  28  f32  dominant_freq_hz PROPOSED: helps the master match events across nodes
//  32  u16  flags            PROTO_EVENT_*
//  34  2    reserved
//  36  u32  event_id         DSP counter, a gap means events were lost
struct proto_event {
    uint64_t t_event_ns;
    uint64_t t_send_ns;
    float    theta;
    float    phi;
    float    confidence;
    float    dominant_freq_hz;
    uint16_t flags;
    uint32_t event_id;
};

// HEARTBEAT (20 bytes)
//   0  u64  t_send_ns    node's CLOCK_REALTIME when sent
//   8  u32  clock_err_us chrony's estimated error, from adjtimex()
//  12  u8   clock_synced 1 if the kernel reports the clock as synced
//  13  u8   dsp_alive    1 if the DSP heartbeat counter moved since last check
//  14  2    reserved
//  16  u32  ring_drops   events the DSP dropped because the ring was full
struct proto_heartbeat {
    uint64_t t_send_ns;
    uint32_t clock_err_us;
    uint8_t  clock_synced;
    uint8_t  dsp_alive;
    uint32_t ring_drops;
};

// Packing: buf must have room for the packed size. Reserved bytes are zeroed.
// Each returns the number of bytes written.
//
// To send a message, pack the payload at buf + PROTO_HEADER_LEN, then pack a
// header with payload_len set to the payload function's return value.
size_t proto_pack_header(uint8_t *buf, const struct proto_header *h);
size_t proto_pack_hello(uint8_t *buf, const struct proto_hello *m);
size_t proto_pack_hello_ack(uint8_t *buf, const struct proto_hello_ack *m);
size_t proto_pack_bye(uint8_t *buf, const struct proto_bye *m);
size_t proto_pack_event(uint8_t *buf, const struct proto_event *m);
size_t proto_pack_heartbeat(uint8_t *buf, const struct proto_heartbeat *m);

// Fill in a header with the current magic and version
struct proto_header proto_make_header(uint8_t msg_type, uint16_t payload_len,
                                      uint16_t node_id, uint32_t seq);

// Unpacking: buf holds PROTO_HEADER_LEN bytes. Checks magic, version, and
// payload_len. Does not check msg_type, so unknown types can be skipped.
enum proto_result proto_unpack_header(const uint8_t *buf, struct proto_header *h);

// Unpacking payloads: len is the header's payload_len, and must match the
// packed size of the message exactly.
enum proto_result proto_unpack_hello(const uint8_t *buf, size_t len, struct proto_hello *m);
enum proto_result proto_unpack_hello_ack(const uint8_t *buf, size_t len, struct proto_hello_ack *m);
enum proto_result proto_unpack_bye(const uint8_t *buf, size_t len, struct proto_bye *m);
enum proto_result proto_unpack_event(const uint8_t *buf, size_t len, struct proto_event *m);
enum proto_result proto_unpack_heartbeat(const uint8_t *buf, size_t len, struct proto_heartbeat *m);

#endif
