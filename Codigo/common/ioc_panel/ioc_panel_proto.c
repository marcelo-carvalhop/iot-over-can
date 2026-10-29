#include "ioc_panel_proto.h"

#include <string.h>

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t ioc_panel_age_encode(uint32_t ms) {
    if (ms <= 0x7FFFu) return (uint16_t)ms;
    uint32_t s = ms / 1000u;
    if (s > 0x7FFFu) s = 0x7FFFu;
    return (uint16_t)(0x8000u | s);
}

uint32_t ioc_panel_age_decode(uint16_t raw) {
    return (raw & 0x8000u) ? (uint32_t)(raw & 0x7FFFu) * 1000u : raw;
}

size_t ioc_panel_msg(uint8_t cmd, const uint8_t *body, size_t body_len, uint8_t *out, size_t cap) {
    if (!out || cap < body_len + 3) return 0;
    put_u16(out, IOC_PANEL_MAGIC);
    out[2] = cmd;
    if (body_len) memcpy(out + 3, body, body_len);
    return body_len + 3;
}

uint8_t ioc_panel_msg_parse(const uint8_t *buf, size_t len, const uint8_t **body, size_t *body_len) {
    if (!buf || len < 3 || get_u16(buf) != IOC_PANEL_MAGIC) return 0;
    if (buf[2] < IOC_PANEL_CMD_FRAMES || buf[2] > IOC_PANEL_CMD_MARK_ACK) return 0;
    if (body) *body = buf + 3;
    if (body_len) *body_len = len - 3;
    return buf[2];
}

size_t ioc_panel_frames_begin(uint8_t *out, size_t cap, const ioc_panel_frames_hdr *hdr) {
    if (!out || !hdr || cap < IOC_PANEL_FRAMES_HEADER) return 0;
    put_u16(out, IOC_PANEL_MAGIC);
    out[2] = IOC_PANEL_CMD_FRAMES;
    out[3] = hdr->flags;
    put_u16(out + 4, hdr->seq);
    put_u16(out + 6, hdr->span_ms);
    put_u32(out + 8, hdr->skipped_bits);
    out[12] = 0;
    return IOC_PANEL_FRAMES_HEADER;
}

size_t ioc_panel_frames_add(uint8_t *out, size_t cap, size_t used, const ioc_panel_frame *frame) {
    if (!out || !frame || frame->len > 8 || used < IOC_PANEL_FRAMES_HEADER || out[12] == 0xFF) return 0;
    const size_t need = 5u + frame->len;
    if (used + need > cap) return 0;
    uint8_t *p = out + used;
    put_u16(p, ioc_panel_age_encode(frame->age_ms));
    put_u16(p + 2, (uint16_t)((frame->id & 0x7FFu) | (frame->own ? 0x8000u : 0u)));
    p[4] = frame->len;
    memcpy(p + 5, frame->data, frame->len);
    out[12]++;
    return used + need;
}

void ioc_panel_frames_set_flags(uint8_t *out, uint8_t flags) { if (out) out[3] = flags; }
uint8_t ioc_panel_frames_count(const uint8_t *out) { return out ? out[12] : 0; }

int ioc_panel_frames_parse(const uint8_t *buf, size_t len, ioc_panel_frames_hdr *hdr, size_t *offset) {
    if (!buf || !hdr || !offset || len < IOC_PANEL_FRAMES_HEADER) return 0;
    if (get_u16(buf) != IOC_PANEL_MAGIC || buf[2] != IOC_PANEL_CMD_FRAMES) return 0;
    hdr->flags = buf[3];
    hdr->seq = get_u16(buf + 4);
    hdr->span_ms = get_u16(buf + 6);
    hdr->skipped_bits = get_u32(buf + 8);
    hdr->count = buf[12];
    *offset = IOC_PANEL_FRAMES_HEADER;
    return 1;
}

int ioc_panel_frames_next(const uint8_t *buf, size_t len, size_t *offset, ioc_panel_frame *frame) {
    if (!buf || !offset || !frame) return -1;
    if (*offset == len) return 0;
    if (*offset + 5 > len) return -1;
    const uint8_t *p = buf + *offset;
    const uint8_t dlc = p[4];
    if (dlc > 8 || *offset + 5u + dlc > len) return -1;
    const uint16_t idf = get_u16(p + 2);
    frame->age_ms = ioc_panel_age_decode(get_u16(p));
    frame->id = (uint16_t)(idf & 0x7FFu);
    frame->own = (idf & 0x8000u) ? 1 : 0;
    frame->len = dlc;
    memset(frame->data, 0, sizeof(frame->data));
    memcpy(frame->data, p + 5, dlc);
    *offset += 5u + dlc;
    return 1;
}

size_t ioc_panel_hello_encode(const ioc_panel_hello *in, uint8_t out[IOC_PANEL_HELLO_LEN]) {
    put_u16(out, IOC_PANEL_MAGIC);
    out[2] = IOC_PANEL_CMD_HELLO;
    out[3] = in->version;
    out[4] = in->flags;
    put_u16(out + 5, in->sleep_s);
    return IOC_PANEL_HELLO_LEN;
}

int ioc_panel_hello_decode(const uint8_t *buf, size_t len, ioc_panel_hello *out) {
    if (!buf || !out || len < IOC_PANEL_HELLO_LEN) return 0;
    if (get_u16(buf) != IOC_PANEL_MAGIC || buf[2] != IOC_PANEL_CMD_HELLO) return 0;
    out->version = buf[3];
    out->flags = buf[4];
    out->sleep_s = get_u16(buf + 5);
    return 1;
}

size_t ioc_panel_status_encode_body(const ioc_panel_status *in, uint8_t out[IOC_PANEL_STATUS_BODY]) {
    out[0] = in->flags;
    out[1] = in->battery_pct;
    put_u16(out + 2, in->battery_mv);
    put_u16(out + 4, (uint16_t)in->temp_c10);
    put_u16(out + 6, in->rh_10);
    put_u16(out + 8, in->sleep_s);
    put_u32(out + 10, in->page_hash);
    put_u16(out + 14, in->page_seq);
    put_u16(out + 16, in->refreshes);
    put_u32(out + 18, in->uptime_s);
    out[22] = in->severity;
    return IOC_PANEL_STATUS_BODY;
}

int ioc_panel_status_decode_body(const uint8_t *buf, size_t len, ioc_panel_status *out) {
    if (!buf || !out || len < IOC_PANEL_STATUS_BODY) return 0;
    out->flags = buf[0];
    out->battery_pct = buf[1];
    out->battery_mv = get_u16(buf + 2);
    out->temp_c10 = (int16_t)get_u16(buf + 4);
    out->rh_10 = get_u16(buf + 6);
    out->sleep_s = get_u16(buf + 8);
    out->page_hash = get_u32(buf + 10);
    out->page_seq = get_u16(buf + 14);
    out->refreshes = get_u16(buf + 16);
    out->uptime_s = get_u32(buf + 18);
    out->severity = buf[22] > 3 ? 3 : buf[22];
    return 1;
}

size_t ioc_panel_mark_encode_body(const ioc_panel_mark *in, uint8_t *out, size_t cap) {
    uint8_t n = in->text_len > IOC_PANEL_MARK_TEXT_MAX ? IOC_PANEL_MARK_TEXT_MAX : in->text_len;
    if (!out || cap < (size_t)IOC_PANEL_MARK_FIXED + n) return 0;
    put_u16(out, in->seq);
    out[2] = in->kind;
    out[3] = in->step;
    put_u32(out + 4, in->age_ms);
    out[8] = n;
    memcpy(out + 9, in->text, n);
    return (size_t)IOC_PANEL_MARK_FIXED + n;
}

int ioc_panel_mark_decode_body(const uint8_t *buf, size_t len, ioc_panel_mark *out) {
    if (!buf || !out || len < IOC_PANEL_MARK_FIXED) return 0;
    const uint8_t n = buf[8];
    if (n > IOC_PANEL_MARK_TEXT_MAX || len < (size_t)IOC_PANEL_MARK_FIXED + n) return 0;
    out->seq = get_u16(buf);
    out->kind = buf[2];
    out->step = buf[3];
    out->age_ms = get_u32(buf + 4);
    out->text_len = n;
    memcpy(out->text, buf + 9, n);
    out->text[n] = '\0';
    return 1;
}

const char *ioc_panel_mark_kind_name(uint8_t kind) {
    switch (kind) {
        case IOC_PANEL_MARK_BUTTON: return "BUTTON";
        case IOC_PANEL_MARK_NOTE: return "NOTE";
        case IOC_PANEL_MARK_STEP: return "STEP";
        case IOC_PANEL_MARK_RESULT: return "RESULT";
        case IOC_PANEL_MARK_FLAG: return "FLAG";
        case IOC_PANEL_MARK_ACK: return "ACK";
        default: return "UNKNOWN";
    }
}
