#include "ioc_wdata.h"

#include <string.h>

#define SEGMENT_HEADER_FIRST 4 /* filho|tipo, transferência|índice, tamanho, CRC */
#define SEGMENT_HEADER_NEXT  2 /* filho|tipo, transferência|índice */

/* ------------------------------------------------------- serialização LE */

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_f32(uint8_t *p, float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    put_u32(p, bits);
}

static float get_f32(const uint8_t *p) {
    uint32_t bits = get_u32(p);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

uint16_t ioc_wd_arg_u16(const uint8_t *arg) { return get_u16(arg); }
float    ioc_wd_arg_f32(const uint8_t arg[4]) { return get_f32(arg); }
void     ioc_wd_put_arg_u16(uint8_t *arg, uint16_t value) { put_u16(arg, value); }
void     ioc_wd_put_arg_f32(uint8_t arg[4], float value) { put_f32(arg, value); }

/* ------------------------------------------------------------ segmentação */

uint8_t ioc_wd_crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8; ++b) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static int valid_frame_len(uint8_t frame_len) {
    switch (frame_len) {
        case 8: case 12: case 16: case 20: case 24: case 32: case 48: case 64:
            return 1;
        default:
            return 0;
    }
}

uint8_t ioc_wd_fd_frame_len(size_t len) {
    static const uint8_t sizes[] = {8, 12, 16, 20, 24, 32, 48, 64};
    for (unsigned i = 0; i < sizeof(sizes); ++i) {
        if (len <= sizes[i]) return sizes[i];
    }
    return 0;
}

uint8_t ioc_wd_segment_count_ex(size_t len, uint8_t frame_len) {
    if (!valid_frame_len(frame_len) || len == 0 || len > IOC_WD_MAX_PAYLOAD) return 0;
    const size_t first = (size_t)frame_len - SEGMENT_HEADER_FIRST;
    const size_t next = (size_t)frame_len - SEGMENT_HEADER_NEXT;
    if (len <= first) return 1;
    size_t count = 1u + (len - first + next - 1u) / next;
    return count > IOC_WD_MAX_SEGMENTS ? 0 : (uint8_t)count;
}

uint8_t ioc_wd_build_segment_ex(uint8_t child, uint8_t kind, uint8_t transfer,
                                const uint8_t *payload, size_t len, uint8_t index,
                                uint8_t frame_len, uint8_t *frame) {
    uint8_t count = ioc_wd_segment_count_ex(len, frame_len);
    if (!payload || !frame || count == 0 || index >= count) return 0;
    if (child > IOC_WD_MAX_CHILD || kind > 0x1Fu) return 0;

    const size_t first = (size_t)frame_len - SEGMENT_HEADER_FIRST;
    const size_t next = (size_t)frame_len - SEGMENT_HEADER_NEXT;
    size_t used;

    memset(frame, 0, frame_len);
    frame[0] = (uint8_t)((child << 5) | (kind & 0x1Fu));
    frame[1] = (uint8_t)(((transfer & 0x07u) << 5) | (index & 0x1Fu));

    if (index == 0) {
        size_t n = len < first ? len : first;
        frame[2] = (uint8_t)len;
        frame[3] = ioc_wd_crc8(payload, len);
        memcpy(&frame[SEGMENT_HEADER_FIRST], payload, n);
        used = SEGMENT_HEADER_FIRST + n;
    } else {
        size_t offset = first + (size_t)(index - 1u) * next;
        size_t n = len - offset;
        if (n > next) n = next;
        memcpy(&frame[SEGMENT_HEADER_NEXT], &payload[offset], n);
        used = SEGMENT_HEADER_NEXT + n;
    }
    /* No CAN clássico todo segmento tem 8 bytes; no FD, o menor tamanho válido. */
    return frame_len == IOC_WD_FRAME_LEN ? IOC_WD_FRAME_LEN : ioc_wd_fd_frame_len(used);
}

uint8_t ioc_wd_segment_count(size_t len) {
    return ioc_wd_segment_count_ex(len, IOC_WD_FRAME_LEN);
}

int ioc_wd_build_segment(uint8_t child, uint8_t kind, uint8_t transfer,
                         const uint8_t *payload, size_t len, uint8_t index,
                         uint8_t frame[IOC_WD_FRAME_LEN]) {
    return ioc_wd_build_segment_ex(child, kind, transfer, payload, len, index,
                                   IOC_WD_FRAME_LEN, frame) != 0;
}

void ioc_wd_rx_reset(ioc_wd_rx *rx) {
    if (rx) memset(rx, 0, sizeof(*rx));
}

static ioc_wd_rx_slot *rx_find(ioc_wd_rx *rx, uint8_t child, uint8_t kind) {
    for (unsigned i = 0; i < IOC_WD_RX_SLOTS; ++i) {
        if (rx->slots[i].active && rx->slots[i].child == child && rx->slots[i].kind == kind) {
            return &rx->slots[i];
        }
    }
    return NULL;
}

static ioc_wd_rx_slot *rx_allocate(ioc_wd_rx *rx, uint32_t now_ms) {
    ioc_wd_rx_slot *oldest = &rx->slots[0];
    for (unsigned i = 0; i < IOC_WD_RX_SLOTS; ++i) {
        ioc_wd_rx_slot *slot = &rx->slots[i];
        if (!slot->active || now_ms - slot->started_ms > IOC_WD_RX_TIMEOUT_MS) return slot;
        if (now_ms - slot->started_ms > now_ms - oldest->started_ms) oldest = slot;
    }
    return oldest; /* todos ocupados e recentes: descarta a transferência mais antiga */
}

ioc_wd_rx_result ioc_wd_rx_push_ex(ioc_wd_rx *rx, const uint8_t *frame, uint8_t frame_len,
                                   uint32_t now_ms, const ioc_wd_rx_slot **done) {
    if (done) *done = NULL;
    if (!rx || !frame || frame_len < IOC_WD_FRAME_LEN || frame_len > IOC_WD_FD_MAX_FRAME_LEN) {
        return IOC_WD_RX_ERROR;
    }

    const uint8_t child = (uint8_t)(frame[0] >> 5);
    const uint8_t kind = (uint8_t)(frame[0] & 0x1Fu);
    const uint8_t transfer = (uint8_t)(frame[1] >> 5);
    const uint8_t index = (uint8_t)(frame[1] & 0x1Fu);
    ioc_wd_rx_slot *slot = rx_find(rx, child, kind);

    if (index == 0) {
        const uint8_t total = frame[2];
        const uint8_t room = (uint8_t)(frame_len - SEGMENT_HEADER_FIRST);
        if (total == 0 || total > IOC_WD_MAX_PAYLOAD) {
            if (slot) slot->active = 0;
            return IOC_WD_RX_ERROR;
        }
        if (!slot) slot = rx_allocate(rx, now_ms);
        slot->active = 1;
        slot->child = child;
        slot->kind = kind;
        slot->transfer = transfer;
        slot->total_len = total;
        slot->crc = frame[3];
        slot->started_ms = now_ms;
        slot->received = total < room ? total : room;
        slot->next_index = 1;
        memcpy(slot->data, &frame[SEGMENT_HEADER_FIRST], slot->received);
    } else {
        if (!slot) return IOC_WD_RX_ERROR;
        if (slot->transfer != transfer || slot->next_index != index ||
            now_ms - slot->started_ms > IOC_WD_RX_TIMEOUT_MS) {
            slot->active = 0;
            return IOC_WD_RX_ERROR;
        }
        const uint8_t room = (uint8_t)(frame_len - SEGMENT_HEADER_NEXT);
        uint8_t remaining = (uint8_t)(slot->total_len - slot->received);
        uint8_t n = remaining < room ? remaining : room;
        if (n == 0) {
            slot->active = 0;
            return IOC_WD_RX_ERROR;
        }
        memcpy(&slot->data[slot->received], &frame[SEGMENT_HEADER_NEXT], n);
        slot->received = (uint8_t)(slot->received + n);
        slot->next_index++;
    }

    if (slot->received < slot->total_len) return IOC_WD_RX_PENDING;

    slot->active = 0;
    if (ioc_wd_crc8(slot->data, slot->total_len) != slot->crc) return IOC_WD_RX_ERROR;
    if (done) *done = slot;
    return IOC_WD_RX_COMPLETE;
}

ioc_wd_rx_result ioc_wd_rx_push(ioc_wd_rx *rx, const uint8_t frame[IOC_WD_FRAME_LEN],
                                uint32_t now_ms, const ioc_wd_rx_slot **done) {
    return ioc_wd_rx_push_ex(rx, frame, IOC_WD_FRAME_LEN, now_ms, done);
}

/* -------------------------------------------------------------- telemetria */

size_t ioc_wd_telemetry_encode(const ioc_wd_telemetry *in, uint8_t out[IOC_WD_TELEMETRY_LEN]) {
    put_u16(&out[0], in->seq);
    out[2] = in->mode;
    out[3] = in->acquisition;
    out[4] = in->axis_mask;
    out[5] = in->flags;
    put_u16(&out[6], in->window_size);
    put_f32(&out[8], in->rate_req_hz);
    put_f32(&out[12], in->rate_eff_hz);
    put_f32(&out[16], in->rms);
    put_f32(&out[20], in->kurtosis);
    put_f32(&out[24], in->crest);
    put_f32(&out[28], in->peak_hz);
    put_f32(&out[32], in->peak_amp);
    put_f32(&out[36], in->entropy);
    put_f32(&out[40], in->ppv_mm_s);
    out[44] = in->battery_pct;
    put_u16(&out[45], in->battery_mv);
    out[47] = in->dtc_count;
    put_u16(&out[48], in->dtc_code);
    return IOC_WD_TELEMETRY_LEN;
}

int ioc_wd_telemetry_decode(const uint8_t *buf, size_t len, ioc_wd_telemetry *out) {
    if (!buf || !out || len != IOC_WD_TELEMETRY_LEN) return 0;
    out->seq = get_u16(&buf[0]);
    out->mode = buf[2];
    out->acquisition = buf[3];
    out->axis_mask = buf[4];
    out->flags = buf[5];
    out->window_size = get_u16(&buf[6]);
    out->rate_req_hz = get_f32(&buf[8]);
    out->rate_eff_hz = get_f32(&buf[12]);
    out->rms = get_f32(&buf[16]);
    out->kurtosis = get_f32(&buf[20]);
    out->crest = get_f32(&buf[24]);
    out->peak_hz = get_f32(&buf[28]);
    out->peak_amp = get_f32(&buf[32]);
    out->entropy = get_f32(&buf[36]);
    out->ppv_mm_s = get_f32(&buf[40]);
    out->battery_pct = buf[44];
    out->battery_mv = get_u16(&buf[45]);
    out->dtc_count = buf[47];
    out->dtc_code = get_u16(&buf[48]);
    return 1;
}

/* ---------------------------------------------------------------- espectro */

size_t ioc_wd_spectrum_encode(const ioc_wd_spectrum_header *hdr, const uint16_t *bins,
                              uint8_t *out, size_t out_cap) {
    if (!hdr || !bins || !out) return 0;
    size_t total = IOC_WD_SPECTRUM_HEADER_LEN + (size_t)hdr->count * 2u;
    if (hdr->count == 0 || total > out_cap || total > IOC_WD_MAX_PAYLOAD) return 0;
    out[0] = hdr->spectrum_id;
    put_u16(&out[1], hdr->total_bins);
    put_u16(&out[3], hdr->offset);
    out[5] = hdr->count;
    put_f32(&out[6], hdr->scale);
    put_f32(&out[10], hdr->sample_rate_hz);
    put_u16(&out[14], hdr->fft_size);
    out[16] = hdr->window_type;
    for (unsigned i = 0; i < hdr->count; ++i) {
        put_u16(&out[IOC_WD_SPECTRUM_HEADER_LEN + i * 2u], bins[i]);
    }
    return total;
}

int ioc_wd_spectrum_decode(const uint8_t *buf, size_t len, ioc_wd_spectrum_header *hdr,
                           const uint8_t **bins_le) {
    if (!buf || !hdr || len < IOC_WD_SPECTRUM_HEADER_LEN) return 0;
    hdr->spectrum_id = buf[0];
    hdr->total_bins = get_u16(&buf[1]);
    hdr->offset = get_u16(&buf[3]);
    hdr->count = buf[5];
    hdr->scale = get_f32(&buf[6]);
    hdr->sample_rate_hz = get_f32(&buf[10]);
    hdr->fft_size = get_u16(&buf[14]);
    hdr->window_type = buf[16];
    if (hdr->count == 0 || len != IOC_WD_SPECTRUM_HEADER_LEN + (size_t)hdr->count * 2u) return 0;
    if ((uint32_t)hdr->offset + hdr->count > hdr->total_bins) return 0;
    if (bins_le) *bins_le = &buf[IOC_WD_SPECTRUM_HEADER_LEN];
    return 1;
}

/* Verdadeiro para valores finitos; NaN e infinito falham sem depender de libm. */
static int is_finite_f32(float v) {
    return (v - v) == 0.0f;
}

void ioc_wd_spectrum_reduce(const float *in, uint16_t in_bins, uint16_t *out, uint16_t out_bins,
                            float *scale) {
    float peak = 0.0f;
    if (scale) *scale = 0.0f;
    if (!in || !out || in_bins == 0 || out_bins == 0) return;
    if (out_bins > in_bins) out_bins = in_bins;

    for (uint16_t i = 0; i < in_bins; ++i) {
        float v = in[i];
        if (is_finite_f32(v) && v > peak) peak = v;
    }
    if (scale) *scale = peak;

    for (uint16_t o = 0; o < out_bins; ++o) {
        uint32_t start = (uint32_t)o * in_bins / out_bins;
        uint32_t end = (uint32_t)(o + 1u) * in_bins / out_bins;
        float band = 0.0f;
        if (end <= start) end = start + 1u;
        for (uint32_t i = start; i < end && i < in_bins; ++i) {
            float v = in[i];
            if (is_finite_f32(v) && v > band) band = v;
        }
        if (peak <= 0.0f || band <= 0.0f) {
            out[o] = 0;
        } else {
            float normalized = band / peak * 65535.0f + 0.5f;
            out[o] = normalized >= 65535.0f ? 65535u : (uint16_t)normalized;
        }
    }
}

/* ------------------------------------------------------- mensagens curtas */

size_t ioc_wd_ack_encode(const ioc_wd_ack *in, uint8_t out[IOC_WD_ACK_LEN]) {
    out[0] = in->seq;
    out[1] = in->opcode;
    out[2] = in->status;
    put_u16(&out[3], in->detail);
    return IOC_WD_ACK_LEN;
}

int ioc_wd_ack_decode(const uint8_t *buf, size_t len, ioc_wd_ack *out) {
    if (!buf || !out || len != IOC_WD_ACK_LEN) return 0;
    out->seq = buf[0];
    out->opcode = buf[1];
    out->status = buf[2];
    out->detail = get_u16(&buf[3]);
    return 1;
}

size_t ioc_wd_dtc_encode(const ioc_wd_dtc *in, uint8_t out[IOC_WD_DTC_LEN]) {
    put_u16(&out[0], in->code);
    out[2] = in->symptom;
    out[3] = in->severity;
    put_u32(&out[4], in->timestamp_ms);
    return IOC_WD_DTC_LEN;
}

int ioc_wd_dtc_decode(const uint8_t *buf, size_t len, ioc_wd_dtc *out) {
    if (!buf || !out || len != IOC_WD_DTC_LEN) return 0;
    out->code = get_u16(&buf[0]);
    out->symptom = buf[2];
    out->severity = buf[3];
    out->timestamp_ms = get_u32(&buf[4]);
    return 1;
}

size_t ioc_wd_link_encode(const ioc_wd_link *in, uint8_t out[IOC_WD_LINK_LEN]) {
    out[0] = in->state;
    out[1] = (uint8_t)in->wifi_rssi;
    put_u16(&out[2], in->session_age_s);
    put_u16(&out[4], in->rx_datagrams);
    put_u16(&out[6], in->auth_failures);
    put_u16(&out[8], in->replay_drops);
    put_u16(&out[10], in->lost_datagrams);
    out[12] = in->stream_enabled;
    put_u16(&out[13], in->stream_period_ms);
    put_u16(&out[15], in->rtt_100us);
    put_u16(&out[17], in->rtt_max_100us);
    return IOC_WD_LINK_LEN;
}

int ioc_wd_link_decode(const uint8_t *buf, size_t len, ioc_wd_link *out) {
    if (!buf || !out || (len != IOC_WD_LINK_LEN && len != IOC_WD_LINK_LEN_V017)) return 0;
    out->state = buf[0];
    out->wifi_rssi = (int8_t)buf[1];
    out->session_age_s = get_u16(&buf[2]);
    out->rx_datagrams = get_u16(&buf[4]);
    out->auth_failures = get_u16(&buf[6]);
    out->replay_drops = get_u16(&buf[8]);
    out->lost_datagrams = get_u16(&buf[10]);
    out->stream_enabled = buf[12];
    out->stream_period_ms = get_u16(&buf[13]);
    out->rtt_100us = len >= IOC_WD_LINK_LEN ? get_u16(&buf[15]) : IOC_WD_RTT_UNKNOWN;
    out->rtt_max_100us = len >= IOC_WD_LINK_LEN ? get_u16(&buf[17]) : IOC_WD_RTT_UNKNOWN;
    return 1;
}

size_t ioc_wd_config_encode(const ioc_wd_config *in, uint8_t out[IOC_WD_CONFIG_LEN]) {
    out[0] = in->mode;
    out[1] = in->window_type;
    put_u16(&out[2], in->window_size);
    put_f32(&out[4], in->rate_req_hz);
    put_f32(&out[8], in->rate_eff_hz);
    put_f32(&out[12], in->stalta);
    put_f32(&out[16], in->gain);
    out[20] = in->status;
    return IOC_WD_CONFIG_LEN;
}

int ioc_wd_config_decode(const uint8_t *buf, size_t len, ioc_wd_config *out) {
    if (!buf || !out || len != IOC_WD_CONFIG_LEN) return 0;
    out->mode = buf[0];
    out->window_type = buf[1];
    out->window_size = get_u16(&buf[2]);
    out->rate_req_hz = get_f32(&buf[4]);
    out->rate_eff_hz = get_f32(&buf[8]);
    out->stalta = get_f32(&buf[12]);
    out->gain = get_f32(&buf[16]);
    out->status = buf[20];
    return 1;
}

/* ----------------------------------------------------------------- comandos */

void ioc_wd_command_encode(const ioc_wd_command *in, uint8_t frame[IOC_WD_FRAME_LEN]) {
    frame[0] = in->seq;
    frame[1] = in->node_id;
    frame[2] = in->child_id;
    frame[3] = in->opcode;
    memcpy(&frame[4], in->arg, 4);
}

void ioc_wd_command_decode(const uint8_t frame[IOC_WD_FRAME_LEN], ioc_wd_command *out) {
    out->seq = frame[0];
    out->node_id = frame[1];
    out->child_id = frame[2];
    out->opcode = frame[3];
    memcpy(out->arg, &frame[4], 4);
}
