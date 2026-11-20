#include "ioc_isolation.h"

#define EFLG_TXEP 0x10u
#define EFLG_TXBO 0x20u
#define TEC_PASSIVE 128u

void ioc_iso_init(ioc_iso *s, uint32_t now_ms) {
    s->last_rx_ms = now_ms;
    s->recover_since_ms = 0;
    s->recovering = 0;
    s->isolated = 0;
}

void ioc_iso_note_rx(ioc_iso *s, uint32_t now_ms) {
    s->last_rx_ms = now_ms;
}

int ioc_iso_tx_failing(uint8_t tec, uint8_t eflg) {
    return tec >= TEC_PASSIVE || (eflg & (EFLG_TXEP | EFLG_TXBO)) != 0;
}

ioc_iso_event ioc_iso_poll(ioc_iso *s, uint32_t now_ms, uint8_t tec, uint8_t eflg,
                           uint32_t quiet_ms, uint32_t recover_ms) {
    const uint32_t since_rx = now_ms - s->last_rx_ms;   /* aritmética modular: vale na virada do contador */

    if (quiet_ms == 0) {
        if (s->isolated) {   /* a detecção foi desligada: não deixa o módulo preso no estado */
            s->isolated = 0;
            s->recovering = 0;
            return IOC_ISO_EXITED;
        }
        return IOC_ISO_NONE;
    }

    if (!s->isolated) {
        if (since_rx >= quiet_ms && ioc_iso_tx_failing(tec, eflg)) {
            s->isolated = 1;
            s->recovering = 0;
            return IOC_ISO_ENTERED;
        }
        return IOC_ISO_NONE;
    }

    if (since_rx >= IOC_ISO_RX_RECENT_MS) {
        s->recovering = 0;   /* a chegada de quadros parou: recomeça a contagem */
        return IOC_ISO_NONE;
    }
    if (!s->recovering) {
        s->recovering = 1;
        s->recover_since_ms = now_ms;
        return IOC_ISO_NONE;
    }
    if ((uint32_t)(now_ms - s->recover_since_ms) >= recover_ms) {
        s->isolated = 0;
        s->recovering = 0;
        return IOC_ISO_EXITED;
    }
    return IOC_ISO_NONE;
}
