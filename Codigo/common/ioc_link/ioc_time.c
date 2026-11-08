#include "ioc_time.h"

#include <stddef.h>

static int valid_source(uint8_t source) {
    return source >= IOC_TIME_SRC_TERMINAL && source <= IOC_TIME_SRC_OPERATOR;
}

void ioc_time_encode(const ioc_time_msg *in, uint8_t out[IOC_TIME_LEN]) {
    const int64_t ms = in->utc_ms > 0 ? in->utc_ms : 0;
    const uint32_t s = (uint32_t)(ms / 1000);
    const uint16_t frac = (uint16_t)(ms % 1000);
    out[0] = in->kind;
    out[1] = in->source;
    out[2] = (uint8_t)s;
    out[3] = (uint8_t)(s >> 8);
    out[4] = (uint8_t)(s >> 16);
    out[5] = (uint8_t)(s >> 24);
    out[6] = (uint8_t)frac;
    out[7] = (uint8_t)(frac >> 8);
}

int ioc_time_decode(const uint8_t *d, uint8_t len, ioc_time_msg *out) {
    if (!d || !out || len != IOC_TIME_LEN) return 0;
    const uint32_t s = (uint32_t)d[2] | ((uint32_t)d[3] << 8) | ((uint32_t)d[4] << 16) | ((uint32_t)d[5] << 24);
    const uint16_t frac = (uint16_t)(d[6] | (d[7] << 8));
    const int has_time = valid_source(d[1]) && s >= IOC_TIME_MIN_EPOCH && frac < 1000u;
    if (d[0] == IOC_TIME_KIND_TIME) {
        if (!has_time) return 0;
    } else if (d[0] != IOC_TIME_KIND_REQUEST) {
        return 0;
    }
    out->kind = d[0];
    out->source = has_time ? d[1] : (uint8_t)IOC_TIME_SRC_NONE;
    out->utc_ms = has_time ? (int64_t)s * 1000 + frac : 0;
    return 1;
}

int ioc_time_adopt(uint8_t mine_source, int64_t mine_ms, uint8_t rx_source, int64_t rx_ms, uint32_t tolerance_ms) {
    if (!valid_source(rx_source) || rx_ms < (int64_t)IOC_TIME_MIN_EPOCH * 1000) return 0;
    if (!valid_source(mine_source)) return 1;
    if (rx_source != mine_source) return rx_source < mine_source;
    const int64_t diff = rx_ms > mine_ms ? rx_ms - mine_ms : mine_ms - rx_ms;
    return diff > (int64_t)tolerance_ms;
}

const char *ioc_time_source_name(uint8_t source) {
    switch (source) {
        case IOC_TIME_SRC_TERMINAL: return "TERMINAL";
        case IOC_TIME_SRC_RTC:      return "RTC";
        case IOC_TIME_SRC_OPERATOR: return "OPERADOR";
        default:                    return "NENHUMA";
    }
}
