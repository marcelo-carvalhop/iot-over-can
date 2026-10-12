/**
 * @file ioc_bindstore.h
 * @brief Tabela de vínculos de um Node gravada na memória não volátil (sem E/S).
 *
 * O Node CAN guarda nesta forma os sensores wireless associados a ele, para
 * retomá-los depois de um reinício (queda de energia, watchdog, ensaio). O
 * firmware grava o resultado de ioc_bs_encode() na NVS do ESP32 e, no boot,
 * só aceita o que ioc_bs_decode() validar.
 *
 * Formato (big-endian):
 *
 *   'I' 'B' | versão | node_id | n | n × (uuid[8] | filho | perfil | protocolo | flags) | CRC-32
 *
 * flags bit 0: reassociação automática autorizada (política AUTO).
 *
 * A decodificação recusa a tabela inteira quando algo não confere: marca,
 * versão, Node (uma placa regravada com outro NODE_ID não herda vínculos),
 * tamanho, CRC, UUID zero, filho fora de 1..IOC_BS_MAX ou repetido, UUID
 * repetido. Um vínculo duvidoso não é retomado; o operador associa de novo.
 */
#ifndef IOC_BINDSTORE_H
#define IOC_BINDSTORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_BS_VERSION     1
#define IOC_BS_MAX         8
#define IOC_BS_HEADER_LEN  5
#define IOC_BS_ENTRY_LEN   12
#define IOC_BS_CRC_LEN     4
#define IOC_BS_MAX_BYTES   (IOC_BS_HEADER_LEN + IOC_BS_MAX * IOC_BS_ENTRY_LEN + IOC_BS_CRC_LEN)

#define IOC_BS_FLAG_AUTO_FAILOVER 0x01

typedef struct {
    uint64_t uuid;
    uint8_t  child_id;
    uint8_t  profile;
    uint8_t  protocol;
    uint8_t  flags;
} ioc_bs_entry;

/** CRC-32 (IEEE 802.3, refletido), o mesmo do zlib. */
uint32_t ioc_bs_crc32(const uint8_t *data, size_t len);

/** Codifica a tabela. Retorna o tamanho escrito ou 0 se `count` ou `cap` não servirem. */
size_t ioc_bs_encode(uint8_t node_id, const ioc_bs_entry *entries, uint8_t count,
                     uint8_t *out, size_t cap);

/**
 * Decodifica e valida. Retorna 1 e preenche `entries`/`*count` quando a tabela
 * é íntegra, deste `node_id` e cabe em `max`; 0 caso contrário (`*count` = 0).
 */
int ioc_bs_decode(const uint8_t *buf, size_t len, uint8_t node_id,
                  ioc_bs_entry *entries, uint8_t max, uint8_t *count);

#ifdef __cplusplus
}
#endif

#endif /* IOC_BINDSTORE_H */
