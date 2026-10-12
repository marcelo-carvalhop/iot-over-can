#include "ioc_bindstore.h"

#include <string.h>

uint32_t ioc_bs_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

size_t ioc_bs_encode(uint8_t node_id, const ioc_bs_entry *entries, uint8_t count,
                     uint8_t *out, size_t cap) {
    const size_t len = IOC_BS_HEADER_LEN + (size_t)count * IOC_BS_ENTRY_LEN + IOC_BS_CRC_LEN;
    if (count > IOC_BS_MAX || !out || cap < len || (count && !entries)) return 0;

    out[0] = 'I';
    out[1] = 'B';
    out[2] = IOC_BS_VERSION;
    out[3] = node_id;
    out[4] = count;
    uint8_t *p = out + IOC_BS_HEADER_LEN;
    for (uint8_t i = 0; i < count; ++i) {
        for (int b = 0; b < 8; ++b) *p++ = (uint8_t)(entries[i].uuid >> (56 - 8 * b));
        *p++ = entries[i].child_id;
        *p++ = entries[i].profile;
        *p++ = entries[i].protocol;
        *p++ = entries[i].flags;
    }
    const uint32_t crc = ioc_bs_crc32(out, (size_t)(p - out));
    *p++ = (uint8_t)(crc >> 24);
    *p++ = (uint8_t)(crc >> 16);
    *p++ = (uint8_t)(crc >> 8);
    *p++ = (uint8_t)crc;
    return len;
}

int ioc_bs_decode(const uint8_t *buf, size_t len, uint8_t node_id,
                  ioc_bs_entry *entries, uint8_t max, uint8_t *count) {
    if (count) *count = 0;
    if (!buf || !count || len < IOC_BS_HEADER_LEN + IOC_BS_CRC_LEN) return 0;
    if (buf[0] != 'I' || buf[1] != 'B' || buf[2] != IOC_BS_VERSION || buf[3] != node_id) return 0;
    const uint8_t n = buf[4];
    if (n > IOC_BS_MAX || n > max || (n && !entries)) return 0;
    if (len != IOC_BS_HEADER_LEN + (size_t)n * IOC_BS_ENTRY_LEN + IOC_BS_CRC_LEN) return 0;

    const size_t body = len - IOC_BS_CRC_LEN;
    const uint32_t stored = ((uint32_t)buf[body] << 24) | ((uint32_t)buf[body + 1] << 16) |
                            ((uint32_t)buf[body + 2] << 8) | (uint32_t)buf[body + 3];
    if (ioc_bs_crc32(buf, body) != stored) return 0;

    const uint8_t *p = buf + IOC_BS_HEADER_LEN;
    for (uint8_t i = 0; i < n; ++i) {
        ioc_bs_entry e;
        e.uuid = 0;
        for (int b = 0; b < 8; ++b) e.uuid = (e.uuid << 8) | *p++;
        e.child_id = *p++;
        e.profile = *p++;
        e.protocol = *p++;
        e.flags = *p++;
        if (e.uuid == 0 || e.child_id == 0 || e.child_id > IOC_BS_MAX) return 0;
        for (uint8_t j = 0; j < i; ++j) {
            if (entries[j].uuid == e.uuid || entries[j].child_id == e.child_id) return 0;
        }
        entries[i] = e;
    }
    *count = n;
    return 1;
}
