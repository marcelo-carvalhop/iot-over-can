/**
 * @file ioc_secpdu.h
 * @brief PDU autenticada para o barramento CAN, no modelo do AUTOSAR SecOC.
 *
 * PREDISPOSIÇÃO: este módulo está implementado e testado, mas nenhum quadro
 * do projeto o utiliza ainda. No CAN clássico o custo é alto (com 8 bytes por
 * quadro, 4 bytes de proteção deixam 4 de carga útil); ele foi escrito para
 * ser ligado na migração para CAN FD, em que 9 bytes de proteção cabem com
 * folga em um quadro de até 64 bytes. Ver
 * Documentacao/arquitetura/predisposicao-can-fd.md.
 *
 * Formato:
 *
 *   carga útil(n) | atualidade truncada(1) | MAC truncado(m)
 *
 *   MAC = HMAC-SHA256(chave, id_dado(2, BE) || carga útil || atualidade completa(8, BE))[0..m-1]
 *
 * O contador de atualidade completo (64 bits) nunca trafega: cada lado mantém
 * o seu, e o receptor o reconstrói a partir do byte menos significativo. Um
 * valor só é aceito se for maior que o último aceito, o que impede repetição.
 * `id_dado` identifica a mensagem (por exemplo, o identificador CAN), para
 * que uma PDU válida não possa ser reapresentada em outro identificador.
 */
#ifndef IOC_SECPDU_H
#define IOC_SECPDU_H

#include <stddef.h>
#include <stdint.h>

#include "ioc_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_SECPDU_FRESHNESS_LEN 1
#define IOC_SECPDU_MAC_CLASSIC   3  /* 24 bits: perfil compacto, 4 bytes de proteção */
#define IOC_SECPDU_MAC_FD        8  /* 64 bits: perfil para CAN FD, 9 bytes de proteção */
#define IOC_SECPDU_MAC_MAX       16
/** Quantos valores à frente do último aceito o receptor tolera (quadros perdidos). */
#define IOC_SECPDU_WINDOW        255u

#define IOC_SECPDU_OVERHEAD(mac_len) ((size_t)IOC_SECPDU_FRESHNESS_LEN + (size_t)(mac_len))

/**
 * Protege `payload`. Incrementa *tx_freshness e o usa nesta PDU. Retorna o
 * tamanho total escrito em `out`, ou 0 se não couber ou se o contador esgotou.
 */
size_t ioc_secpdu_protect(const uint8_t key[IOC_LINK_KEY_LEN], uint16_t data_id,
                          const uint8_t *payload, size_t payload_len,
                          uint64_t *tx_freshness, uint8_t mac_len,
                          uint8_t *out, size_t out_cap);

/**
 * Verifica uma PDU. Em sucesso retorna 1, atualiza *rx_freshness e informa o
 * tamanho da carga útil (que ocupa o início de `pdu`). Em falha retorna 0 e
 * não altera *rx_freshness.
 */
int ioc_secpdu_verify(const uint8_t key[IOC_LINK_KEY_LEN], uint16_t data_id,
                      const uint8_t *pdu, size_t pdu_len,
                      uint64_t *rx_freshness, uint8_t mac_len,
                      size_t *payload_len);

/** Chave do barramento: HMAC(K_master, "ioc-can-v1"). */
void ioc_secpdu_derive_bus_key(const uint8_t master[IOC_LINK_KEY_LEN],
                               uint8_t out[IOC_LINK_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* IOC_SECPDU_H */
