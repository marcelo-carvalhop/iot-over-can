/**
 * @file ioc_resetlog.h
 * @brief Motivo e contagem dos reinícios de um Node (sem E/S).
 *
 * Um Node que reinicia sozinho (queda de tensão, watchdog, falha do programa)
 * acende os três LEDs a cada partida e parece "piscar". Para separar isso de
 * ruído elétrico, o firmware registra o motivo de cada partida e conta os
 * reinícios anormais na NVS. Esta biblioteca só classifica o motivo, mantém os
 * contadores e os codifica; ler o motivo (esp_reset_reason) e gravar na NVS é
 * com o firmware.
 *
 * Os códigos de motivo são os de esp_reset_reason_t do ESP-IDF (0 a 10), que
 * não mudaram entre as versões. Só os reinícios anormais são contados e
 * gravados: ligar a placa, apertar EN ou abrir a porta serial (DTR/RTS) não
 * desgastam a memória flash.
 *
 * Formato dos contadores (big-endian):
 *
 *   'I' 'R' | versão | último motivo anormal | brownout[4] | watchdog[4] | pânico[4] | CRC-32
 */
#ifndef IOC_RESETLOG_H
#define IOC_RESETLOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_RL_VERSION   1
#define IOC_RL_BYTES     (4 + 3 * 4 + 4)   /* marca(2), versão, último, 3 contadores, CRC */

/* Valores de esp_reset_reason_t. */
#define IOC_RST_UNKNOWN    0
#define IOC_RST_POWERON    1   /* energia ligada (também o EN do kit de desenvolvimento) */
#define IOC_RST_EXT        2   /* pino de reset externo */
#define IOC_RST_SW         3   /* esp_restart() */
#define IOC_RST_PANIC      4   /* exceção ou abort() */
#define IOC_RST_INT_WDT    5   /* watchdog de interrupção */
#define IOC_RST_TASK_WDT   6   /* watchdog de tarefa */
#define IOC_RST_WDT        7   /* outro watchdog */
#define IOC_RST_DEEPSLEEP  8
#define IOC_RST_BROWNOUT   9   /* tensão abaixo do limiar do detector */
#define IOC_RST_SDIO       10

typedef enum {
    IOC_RL_NORMAL = 0,     /* partida esperada: energia, EN, DTR/RTS, esp_restart, sono profundo */
    IOC_RL_BROWNOUT,       /* queda de tensão: problema de alimentação */
    IOC_RL_WATCHDOG,       /* o programa travou */
    IOC_RL_PANIC           /* o programa falhou */
} ioc_rl_class;

typedef struct {
    uint32_t brownout;
    uint32_t watchdog;
    uint32_t panic;
    uint8_t  last;         /* último motivo anormal (IOC_RST_*); 0 se nunca houve */
} ioc_rl_counters;

/** Classe do motivo. Códigos desconhecidos contam como partida normal. */
ioc_rl_class ioc_rl_classify(uint8_t reason);

/** Nome sem acentos, para o monitor serial: "QUEDA_DE_TENSAO", "ENERGIA_LIGADA"... */
const char *ioc_rl_reason_name(uint8_t reason);

/**
 * Registra uma partida. Retorna 1 se os contadores mudaram (reinício anormal) e
 * precisam ser gravados, 0 se a partida foi normal.
 */
int ioc_rl_note(ioc_rl_counters *counters, uint8_t reason);

/** Codifica os contadores. Retorna IOC_RL_BYTES ou 0 se `cap` não servir. */
size_t ioc_rl_encode(const ioc_rl_counters *counters, uint8_t *out, size_t cap);

/** Decodifica e valida (marca, versão, tamanho, CRC). Retorna 1 se íntegro; 0 zera `counters`. */
int ioc_rl_decode(const uint8_t *buf, size_t len, ioc_rl_counters *counters);

/**
 * Escreve a linha de diagnóstico, sem o prefixo do Node:
 *   "reinicio=QUEDA_DE_TENSAO quedas_de_tensao=3 watchdog=0 panico=0"
 * Retorna o tamanho escrito (ou o que caberia, como snprintf).
 */
size_t ioc_rl_format(char *out, size_t cap, uint8_t reason, const ioc_rl_counters *counters);

#ifdef __cplusplus
}
#endif

#endif /* IOC_RESETLOG_H */
