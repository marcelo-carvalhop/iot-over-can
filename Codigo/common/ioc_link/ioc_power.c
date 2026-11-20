#include "ioc_power.h"

#include <stdio.h>
#include <string.h>

uint32_t ioc_pw_divider_mv(uint32_t pin_mv, uint32_t r1_ohm, uint32_t r2_ohm) {
    if (r2_ohm == 0) return 0;
    const uint64_t mv = (uint64_t)pin_mv * ((uint64_t)r1_ohm + r2_ohm) / r2_ohm;
    return mv > 65535u ? 65535u : (uint32_t)mv;
}

int32_t ioc_pw_ina_shunt_uv(uint16_t reg) {
    return (int32_t)(int16_t)reg * 10;
}

int32_t ioc_pw_ina_current_ma(int32_t shunt_uv, uint32_t shunt_mohm) {
    if (shunt_mohm == 0) return 0;
    /* uV / mΩ = mA; arredonda para o inteiro mais próximo, também para correntes negativas. */
    const int32_t half = (int32_t)(shunt_mohm / 2u);
    return shunt_uv >= 0 ? (shunt_uv + half) / (int32_t)shunt_mohm : (shunt_uv - half) / (int32_t)shunt_mohm;
}

void ioc_pw_init(ioc_pw *p, const ioc_pw_cfg *cfg) {
    memset(p, 0, sizeof(*p));
    p->cfg = *cfg;
}

void ioc_pw_clear(ioc_pw *p) {
    const ioc_pw_cfg cfg = p->cfg;
    ioc_pw_init(p, &cfg);
}

int ioc_pw_due(ioc_pw *p, uint32_t now_ms) {
    if (p->sampled_once && (uint32_t)(now_ms - p->last_sample_ms) < IOC_PW_PERIOD_MS) return 0;
    p->sampled_once = 1;
    p->last_sample_ms = now_ms;
    return 1;
}

void ioc_pw_add_pin_mv(ioc_pw *p, uint32_t pin_mv) {
    if (!p->cfg.vin_enabled) return;
    p->ring[p->head] = (uint16_t)ioc_pw_divider_mv(pin_mv, p->cfg.r1_ohm, p->cfg.r2_ohm);
    p->head = (uint8_t)((p->head + 1u) % IOC_PW_WINDOW);
    if (p->count < IOC_PW_WINDOW) ++p->count;
    ++p->samples;
    if (p->count < IOC_PW_WINDOW) return;   /* só há média com a janela cheia */

    uint32_t sum = 0;
    for (unsigned i = 0; i < IOC_PW_WINDOW; ++i) sum += p->ring[i];
    p->avg_mv = (uint16_t)((sum + IOC_PW_WINDOW / 2u) / IOC_PW_WINDOW);
    if (!p->have_avg) {
        p->have_avg = 1;
        p->min_mv = p->max_mv = p->avg_mv;
    } else {
        if (p->avg_mv < p->min_mv) p->min_mv = p->avg_mv;
        if (p->avg_mv > p->max_mv) p->max_mv = p->avg_mv;
    }
}

void ioc_pw_add_shunt_reg(ioc_pw *p, uint16_t reg) {
    if (p->cfg.shunt_mohm == 0) return;
    p->current_seen = 1;
    p->current_ok = 1;
    p->current_ma = ioc_pw_ina_current_ma(ioc_pw_ina_shunt_uv(reg), p->cfg.shunt_mohm);
    if (p->current_ma > p->current_max_ma) p->current_max_ma = p->current_ma;
}

void ioc_pw_current_failed(ioc_pw *p) {
    if (p->cfg.shunt_mohm == 0) return;
    p->current_seen = 1;
    p->current_ok = 0;
}

ioc_pw_level ioc_pw_level_now(const ioc_pw *p) {
    if (!p->cfg.vin_enabled || !p->have_avg) return IOC_PW_NO_READING;
    if (p->avg_mv < p->cfg.crit_mv) return IOC_PW_CRITICAL;
    if (p->avg_mv < p->cfg.warn_mv) return IOC_PW_LOW;
    return IOC_PW_OK;
}

const char *ioc_pw_level_name(ioc_pw_level level) {
    switch (level) {
        case IOC_PW_OK: return "OK";
        case IOC_PW_LOW: return "BAIXA";
        case IOC_PW_CRITICAL: return "CRITICA";
        default: return "SEM_LEITURA";
    }
}

int ioc_pw_warning_due(ioc_pw *p, uint32_t now_ms) {
    const ioc_pw_level level = ioc_pw_level_now(p);
    if (level != IOC_PW_LOW && level != IOC_PW_CRITICAL) return 0;
    if (p->warned_once && (uint32_t)(now_ms - p->last_warn_ms) < IOC_PW_WARN_PERIOD_MS) return 0;
    p->warned_once = 1;
    p->last_warn_ms = now_ms;
    return 1;
}

size_t ioc_pw_format(const ioc_pw *p, char *out, size_t cap) {
    if (cap == 0) return 0;
    size_t len = 0;
    out[0] = '\0';
    if (p->cfg.vin_enabled) {
        int n;
        if (p->have_avg) {
            n = snprintf(out + len, cap - len, " vin_mv=%u vin_min_mv=%u vin_max_mv=%u vin=%s", (unsigned)p->avg_mv,
                         (unsigned)p->min_mv, (unsigned)p->max_mv, ioc_pw_level_name(ioc_pw_level_now(p)));
        } else {
            n = snprintf(out + len, cap - len, " vin=%s", ioc_pw_level_name(IOC_PW_NO_READING));
        }
        if (n > 0) len = (size_t)n >= cap - len ? cap - 1 : len + (size_t)n;
    }
    if (p->cfg.shunt_mohm != 0 && len + 1 < cap) {
        int n;
        if (p->current_seen && p->current_ok) {
            n = snprintf(out + len, cap - len, " corrente_ma=%ld corrente_max_ma=%ld", (long)p->current_ma,
                         (long)p->current_max_ma);
        } else {
            n = snprintf(out + len, cap - len, " corrente=%s", p->current_seen ? "SEM_RESPOSTA" : "SEM_LEITURA");
        }
        if (n > 0) len = (size_t)n >= cap - len ? cap - 1 : len + (size_t)n;
    }
    return len;
}
