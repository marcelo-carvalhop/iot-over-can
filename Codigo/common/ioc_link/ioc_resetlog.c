#include "ioc_resetlog.h"

#include <stdio.h>
#include <string.h>

#include "ioc_bindstore.h"   /* ioc_bs_crc32 */

ioc_rl_class ioc_rl_classify(uint8_t reason) {
    switch (reason) {
        case IOC_RST_BROWNOUT:
            return IOC_RL_BROWNOUT;
        case IOC_RST_INT_WDT:
        case IOC_RST_TASK_WDT:
        case IOC_RST_WDT:
            return IOC_RL_WATCHDOG;
        case IOC_RST_PANIC:
            return IOC_RL_PANIC;
        default:
            return IOC_RL_NORMAL;
    }
}

const char *ioc_rl_reason_name(uint8_t reason) {
    switch (reason) {
        case IOC_RST_POWERON:   return "ENERGIA_LIGADA";
        case IOC_RST_EXT:       return "RESET_EXTERNO";
        case IOC_RST_SW:        return "REINICIO_POR_SOFTWARE";
        case IOC_RST_PANIC:     return "FALHA_DO_PROGRAMA";
        case IOC_RST_INT_WDT:   return "WATCHDOG_DE_INTERRUPCAO";
        case IOC_RST_TASK_WDT:  return "WATCHDOG_DE_TAREFA";
        case IOC_RST_WDT:       return "WATCHDOG";
        case IOC_RST_DEEPSLEEP: return "SONO_PROFUNDO";
        case IOC_RST_BROWNOUT:  return "QUEDA_DE_TENSAO";
        case IOC_RST_SDIO:      return "SDIO";
        default:                return "DESCONHECIDO";
    }
}

int ioc_rl_note(ioc_rl_counters *counters, uint8_t reason) {
    if (!counters) return 0;
    switch (ioc_rl_classify(reason)) {
        case IOC_RL_BROWNOUT:
            if (counters->brownout != UINT32_MAX) counters->brownout++;
            break;
        case IOC_RL_WATCHDOG:
            if (counters->watchdog != UINT32_MAX) counters->watchdog++;
            break;
        case IOC_RL_PANIC:
            if (counters->panic != UINT32_MAX) counters->panic++;
            break;
        default:
            return 0;
    }
    counters->last = reason;
    return 1;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

size_t ioc_rl_encode(const ioc_rl_counters *counters, uint8_t *out, size_t cap) {
    if (!counters || !out || cap < IOC_RL_BYTES) return 0;
    out[0] = 'I';
    out[1] = 'R';
    out[2] = IOC_RL_VERSION;
    out[3] = counters->last;
    put32(out + 4, counters->brownout);
    put32(out + 8, counters->watchdog);
    put32(out + 12, counters->panic);
    put32(out + 16, ioc_bs_crc32(out, 16));
    return IOC_RL_BYTES;
}

int ioc_rl_decode(const uint8_t *buf, size_t len, ioc_rl_counters *counters) {
    if (counters) memset(counters, 0, sizeof(*counters));
    if (!buf || !counters || len != IOC_RL_BYTES) return 0;
    if (buf[0] != 'I' || buf[1] != 'R' || buf[2] != IOC_RL_VERSION) return 0;
    if (ioc_bs_crc32(buf, 16) != get32(buf + 16)) return 0;
    counters->last = buf[3];
    counters->brownout = get32(buf + 4);
    counters->watchdog = get32(buf + 8);
    counters->panic = get32(buf + 12);
    return 1;
}

size_t ioc_rl_format(char *out, size_t cap, uint8_t reason, const ioc_rl_counters *counters) {
    ioc_rl_counters zero;
    memset(&zero, 0, sizeof(zero));
    const ioc_rl_counters *c = counters ? counters : &zero;
    const int n = snprintf(out, cap, "reinicio=%s quedas_de_tensao=%lu watchdog=%lu panico=%lu",
                           ioc_rl_reason_name(reason), (unsigned long)c->brownout,
                           (unsigned long)c->watchdog, (unsigned long)c->panic);
    return n < 0 ? 0 : (size_t)n;
}
