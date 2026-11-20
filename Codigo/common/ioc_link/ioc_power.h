/**
 * @file ioc_power.h
 * @brief Medição da alimentação do módulo: tensão de entrada e corrente (sem E/S).
 *
 * Os reinícios por queda de tensão (ioc_resetlog.h) dizem que a alimentação falhou, mas não quanto. Esta
 * biblioteca acompanha dois valores que o firmware lê por fora:
 *
 *   - a tensão de entrada, por um divisor resistivo ligado a um pino do conversor analógico (ADC1 do ESP32):
 *         Vin ── R1 ──┬── R2 ── GND          Vin = Vpino · (R1 + R2) / R2
 *                     └─ pino do ADC
 *   - a corrente, opcionalmente, pelo INA219 (resistor de derivação de 0,1 Ω nas placas comuns), lendo o
 *     registrador de tensão da derivação (0x01, passos de 10 µV, com sinal): I = Vderivação / R.
 *
 * Só mede e informa: nada no firmware muda de comportamento por causa dos valores. O acompanhamento é feito
 * sobre a média das últimas IOC_PW_WINDOW amostras, para o ruído do conversor do ESP32 (dezenas de mV) não
 * virar um mínimo falso; por isso ele mostra o patamar da tensão e não picos de microssegundos, que só um
 * osciloscópio mostra.
 */
#ifndef IOC_POWER_H
#define IOC_POWER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_PW_WINDOW        16u     /* amostras na média móvel */
#define IOC_PW_PERIOD_MS     10u     /* uma amostra a cada 10 ms (100 por segundo) */
#define IOC_PW_WARN_PERIOD_MS 10000u /* o aviso de tensão baixa sai no máximo a cada 10 s */

typedef struct {
    uint32_t r1_ohm;        /* divisor: Vin ── R1 ──┬── R2 ── GND (o pino do ADC fica em ┬) */
    uint32_t r2_ohm;
    uint32_t warn_mv;       /* abaixo disto a tensão de entrada é BAIXA */
    uint32_t crit_mv;       /* abaixo disto é CRITICA */
    uint32_t shunt_mohm;    /* resistor de derivação do INA219 em mΩ; 0 = sem medição de corrente */
    uint8_t  vin_enabled;   /* 0 = sem medição de tensão */
} ioc_pw_cfg;

typedef enum {
    IOC_PW_NO_READING = 0,  /* ainda não há média (menos de IOC_PW_WINDOW amostras) */
    IOC_PW_OK,
    IOC_PW_LOW,
    IOC_PW_CRITICAL
} ioc_pw_level;

typedef struct {
    ioc_pw_cfg cfg;
    uint16_t ring[IOC_PW_WINDOW];
    uint8_t  count;
    uint8_t  head;
    uint16_t avg_mv;
    uint16_t min_mv;        /* da média móvel, desde a última limpeza */
    uint16_t max_mv;
    uint8_t  have_avg;
    uint32_t samples;
    uint32_t last_sample_ms;
    uint8_t  sampled_once;
    uint32_t last_warn_ms;
    uint8_t  warned_once;
    int32_t  current_ma;
    int32_t  current_max_ma;
    uint8_t  current_ok;    /* a última leitura do INA219 teve resposta */
    uint8_t  current_seen;  /* houve ao menos uma tentativa */
} ioc_pw;

/* Conversões puras. */
uint32_t ioc_pw_divider_mv(uint32_t pin_mv, uint32_t r1_ohm, uint32_t r2_ohm);   /* 0 se R2 = 0; limitado a 65535 */
int32_t  ioc_pw_ina_shunt_uv(uint16_t reg);                                      /* registrador 0x01 */
int32_t  ioc_pw_ina_current_ma(int32_t shunt_uv, uint32_t shunt_mohm);           /* uV / mΩ = mA; 0 se mΩ = 0 */

void ioc_pw_init(ioc_pw *p, const ioc_pw_cfg *cfg);
void ioc_pw_clear(ioc_pw *p);   /* ENERGIA LIMPAR: zera extremos e média, mantém a configuração */

/* 1 quando passou IOC_PW_PERIOD_MS desde a última amostra (a primeira chamada devolve 1). */
int ioc_pw_due(ioc_pw *p, uint32_t now_ms);

void ioc_pw_add_pin_mv(ioc_pw *p, uint32_t pin_mv);    /* tensão lida no pino do ADC */
void ioc_pw_add_shunt_reg(ioc_pw *p, uint16_t reg);    /* registrador 0x01 do INA219 */
void ioc_pw_current_failed(ioc_pw *p);                 /* o INA219 não respondeu */

ioc_pw_level ioc_pw_level_now(const ioc_pw *p);
const char *ioc_pw_level_name(ioc_pw_level level);

/* 1 se a tensão está abaixo do aviso e o último aviso foi há mais de IOC_PW_WARN_PERIOD_MS. */
int ioc_pw_warning_due(ioc_pw *p, uint32_t now_ms);

/* " vin_mv=4987 vin_min_mv=4912 vin_max_mv=5031 vin=OK corrente_ma=143 corrente_max_ma=412" (campos só dos
 * valores ligados); devolve o tamanho escrito, sem o terminador. */
size_t ioc_pw_format(const ioc_pw *p, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
