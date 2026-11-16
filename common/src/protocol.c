// Pack/unpack for the node to master wire protocol
//
// Bytes are written with shifts, so the output is big-endian on any host and
// buffers never need to be aligned.
#include "common/protocol.h"

#include <string.h>

_Static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32 bits");

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    put_u16(p, (uint16_t)(v >> 16));
    put_u16(p + 2, (uint16_t)v);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)(v >> 32));
    put_u32(p + 4, (uint32_t)v);
}

static void put_f32(uint8_t *p, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    put_u32(p, bits);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)get_u16(p) << 16) | get_u16(p + 2);
}

static uint64_t get_u64(const uint8_t *p)
{
    return ((uint64_t)get_u32(p) << 32) | get_u32(p + 4);
}

static float get_f32(const uint8_t *p)
{
    uint32_t bits = get_u32(p);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

struct proto_header proto_make_header(uint8_t msg_type, uint16_t payload_len,
                                      uint16_t node_id, uint32_t seq)
{
    struct proto_header h = {
        .magic = PROTO_MAGIC,
        .version = PROTO_VERSION,
        .msg_type = msg_type,
        .payload_len = payload_len,
        .node_id = node_id,
        .seq = seq,
    };
    return h;
}

size_t proto_pack_header(uint8_t *buf, const struct proto_header *h)
{
    put_u16(buf + 0, h->magic);
    buf[2] = h->version;
    buf[3] = h->msg_type;
    put_u16(buf + 4, h->payload_len);
    put_u16(buf + 6, h->node_id);
    put_u32(buf + 8, h->seq);
    return PROTO_HEADER_LEN;
}

size_t proto_pack_hello(uint8_t *buf, const struct proto_hello *m)
{
    memset(buf, 0, PROTO_HELLO_LEN);
    buf[0] = m->mic_count;
    put_u32(buf + 4, m->build_id);
    return PROTO_HELLO_LEN;
}

size_t proto_pack_hello_ack(uint8_t *buf, const struct proto_hello_ack *m)
{
    memset(buf, 0, PROTO_HELLO_ACK_LEN);
    buf[0] = m->status;
    put_u16(buf + 2, m->heartbeat_ms);
    return PROTO_HELLO_ACK_LEN;
}

size_t proto_pack_bye(uint8_t *buf, const struct proto_bye *m)
{
    memset(buf, 0, PROTO_BYE_LEN);
    buf[0] = m->reason;
    return PROTO_BYE_LEN;
}

size_t proto_pack_event(uint8_t *buf, const struct proto_event *m)
{
    memset(buf, 0, PROTO_EVENT_LEN);
    put_u64(buf + 0, m->t_event_ns);
    put_u64(buf + 8, m->t_send_ns);
    put_f32(buf + 16, m->theta);
    put_f32(buf + 20, m->phi);
    put_f32(buf + 24, m->confidence);
    put_f32(buf + 28, m->dominant_freq_hz);
    put_u16(buf + 32, m->flags);
    put_u32(buf + 36, m->event_id);
    return PROTO_EVENT_LEN;
}

size_t proto_pack_heartbeat(uint8_t *buf, const struct proto_heartbeat *m)
{
    memset(buf, 0, PROTO_HEARTBEAT_LEN);
    put_u64(buf + 0, m->t_send_ns);
    put_u32(buf + 8, m->clock_err_us);
    buf[12] = m->clock_synced;
    buf[13] = m->dsp_alive;
    put_u32(buf + 16, m->ring_drops);
    return PROTO_HEARTBEAT_LEN;
}

enum proto_result proto_unpack_header(const uint8_t *buf, struct proto_header *h)
{
    h->magic = get_u16(buf + 0);
    h->version = buf[2];
    h->msg_type = buf[3];
    h->payload_len = get_u16(buf + 4);
    h->node_id = get_u16(buf + 6);
    h->seq = get_u32(buf + 8);

    if (h->magic != PROTO_MAGIC) {
        return PROTO_ERR_MAGIC;
    }
    if (h->version != PROTO_VERSION) {
        return PROTO_ERR_VERSION;
    }
    if (h->payload_len > PROTO_MAX_PAYLOAD) {
        return PROTO_ERR_LEN;
    }
    return PROTO_OK;
}

enum proto_result proto_unpack_hello(const uint8_t *buf, size_t len, struct proto_hello *m)
{
    if (len != PROTO_HELLO_LEN) {
        return PROTO_ERR_LEN;
    }
    m->mic_count = buf[0];
    m->build_id = get_u32(buf + 4);
    return PROTO_OK;
}

enum proto_result proto_unpack_hello_ack(const uint8_t *buf, size_t len, struct proto_hello_ack *m)
{
    if (len != PROTO_HELLO_ACK_LEN) {
        return PROTO_ERR_LEN;
    }
    m->status = buf[0];
    m->heartbeat_ms = get_u16(buf + 2);
    return PROTO_OK;
}

enum proto_result proto_unpack_bye(const uint8_t *buf, size_t len, struct proto_bye *m)
{
    if (len != PROTO_BYE_LEN) {
        return PROTO_ERR_LEN;
    }
    m->reason = buf[0];
    return PROTO_OK;
}

enum proto_result proto_unpack_event(const uint8_t *buf, size_t len, struct proto_event *m)
{
    if (len != PROTO_EVENT_LEN) {
        return PROTO_ERR_LEN;
    }
    m->t_event_ns = get_u64(buf + 0);
    m->t_send_ns = get_u64(buf + 8);
    m->theta = get_f32(buf + 16);
    m->phi = get_f32(buf + 20);
    m->confidence = get_f32(buf + 24);
    m->dominant_freq_hz = get_f32(buf + 28);
    m->flags = get_u16(buf + 32);
    m->event_id = get_u32(buf + 36);
    return PROTO_OK;
}

enum proto_result proto_unpack_heartbeat(const uint8_t *buf, size_t len, struct proto_heartbeat *m)
{
    if (len != PROTO_HEARTBEAT_LEN) {
        return PROTO_ERR_LEN;
    }
    m->t_send_ns = get_u64(buf + 0);
    m->clock_err_us = get_u32(buf + 8);
    m->clock_synced = buf[12];
    m->dsp_alive = buf[13];
    m->ring_drops = get_u32(buf + 16);
    return PROTO_OK;
}
