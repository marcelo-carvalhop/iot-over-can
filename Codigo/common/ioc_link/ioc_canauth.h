/**
 * @file ioc_canauth.h
 * @brief Autenticação de origem dos comandos do operador no barramento CAN (sem E/S).
 *
 * Qualquer dispositivo ligado ao CAN pode transmitir qualquer identificador: um comando de sensor (0x304) ou de
 * ensaio de falha (0x305) forjado seria obedecido pelos módulos. Esta biblioteca faz a Probe 00 provar a origem
 * de cada um deles com uma etiqueta HMAC-SHA256 (ioc_secpdu.h), no modelo do AUTOSAR SecOC.
 *
 * Só a Probe 00 transmite 0x304 e 0x305, e é por isso que eles cabem neste modelo: o receptor sabe de quem é
 * a atualidade (um contador por emissor). O 0x080 (controle) não cabe: há vários emissores, o quadro não diz
 * qual, e proteger só uma parte dele deixaria o resto forjável. Ver «Autenticação de origem» em seguranca.md.
 *
 * Como os comandos já usam os 8 bytes do quadro, a prova segue num segundo quadro, a etiqueta:
 *
 *   comando   id 0x304 ou 0x305          8 bytes, como sempre
 *   etiqueta  id IOC_CA_TAG_ID (0x307)   tipo(1) | atualidade(2, big-endian) | MAC(5)
 *
 *   MAC = HMAC-SHA256(chave do barramento, id do comando(2) || 8 bytes do comando || atualidade completa(8))[0..4]
 *
 * O receptor guarda o comando e só o executa quando a etiqueta chega (até IOC_CA_TAG_WAIT_MS depois) e confere.
 * A etiqueta seguir o comando é garantido pela fila da Probe, que mantém a ordem. A atualidade completa
 * (64 bits) nunca trafega; o receptor a reconstrói a partir dos 16 bits baixos e só aceita valor maior que o
 * último aceito, até 65535 à frente: repetir um comando e etiqueta já aceitos não funciona.
 *
 * A chave do barramento é HMAC(K_master, "ioc-can-v1") (ioc_secpdu_derive_bus_key). Todos os módulos a têm, e
 * por isso a prova é de que o quadro veio de quem tem a chave, e não de qual módulo. Quem lê a chave de um
 * módulo forja comandos; quem só liga um dispositivo ao cabo não.
 *
 * A atualidade precisa sobreviver a reinícios. A Probe grava um teto e recomeça acima dele, de modo que nunca
 * repete um valor (um reinício pula até IOC_CA_TX_SAVE_STEP valores). O módulo grava o último aceito a cada
 * IOC_CA_RX_SAVE_STEP comandos; depois de um reinício ele volta ao valor gravado e, por isso, aceita de novo
 * até IOC_CA_RX_SAVE_STEP - 1 comandos que já tinha aceitado: é o resíduo de repetição deste desenho. Um módulo
 * sem nada gravado só entra em sincronia se a Probe estiver a menos de 65535 comandos do zero; fora disso, e
 * quando a Probe é trocada (o contador dela recomeça do zero), o operador acerta o módulo pela serial dele
 * (CANAUTH SYNC <valor>), que exige acesso físico.
 */
#ifndef IOC_CANAUTH_H
#define IOC_CANAUTH_H

#include <stddef.h>
#include <stdint.h>

#include "ioc_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_CA_TAG_ID        0x307u
#define IOC_CA_ID_DATA_CMD   0x304u
#define IOC_CA_ID_TEST_CMD   0x305u
#define IOC_CA_KINDS         2u
#define IOC_CA_FRESH_LEN     2u
#define IOC_CA_MAC_LEN       5u
#define IOC_CA_TAG_LEN       8u      /* tipo(1) + atualidade(2) + MAC(5) */
#define IOC_CA_TAG_WAIT_MS   100u
#define IOC_CA_TX_SAVE_STEP  32u     /* a Probe grava um teto 32 valores acima do usado */
#define IOC_CA_RX_SAVE_STEP  8u      /* o módulo grava o último aceito a cada 8 comandos */

/* Tipo do comando protegido (0 = 0x304, 1 = 0x305); -1 se o identificador não é protegido. */
int ioc_ca_kind_of(uint16_t can_id);

/* ---- emissor (Probe 00) ---- */

typedef struct {
    uint8_t  key[IOC_LINK_KEY_LEN];   /* chave do barramento */
    uint64_t freshness;               /* último valor usado */
} ioc_ca_tx;

/* `saved` é o teto gravado: nada acima dele foi usado, e o primeiro valor desta partida é saved + 1. */
void ioc_ca_tx_init(ioc_ca_tx *tx, const uint8_t bus_key[IOC_LINK_KEY_LEN], uint64_t saved);

/* O teto a gravar logo depois de ioc_ca_tx_init. */
uint64_t ioc_ca_tx_boot_ceiling(const ioc_ca_tx *tx);

/* 1 se o próximo valor passaria do teto gravado: grave ioc_ca_tx_next_ceiling() antes de ioc_ca_tx_make_tag. */
int ioc_ca_tx_needs_save(const ioc_ca_tx *tx, uint64_t saved);
uint64_t ioc_ca_tx_next_ceiling(const ioc_ca_tx *tx);

/* Monta a etiqueta de um comando (id 0x304 ou 0x305, 8 bytes). Devolve 1, ou 0 se o quadro não é protegido. */
int ioc_ca_tx_make_tag(ioc_ca_tx *tx, uint16_t can_id, const uint8_t *data, uint8_t len,
                       uint8_t tag[IOC_CA_TAG_LEN]);

/* ---- receptor (módulos) ---- */

typedef struct {
    uint8_t  valid;
    uint8_t  len;
    uint8_t  data[8];
    uint32_t at_ms;
} ioc_ca_pending;

typedef struct {
    uint8_t  key[IOC_LINK_KEY_LEN];
    uint64_t freshness;               /* último valor aceito */
    ioc_ca_pending pending[IOC_CA_KINDS];
    uint32_t accepted;                /* comandos executados */
    uint32_t bad_tag;                 /* etiqueta que não confere (MAC errado ou atualidade velha) */
    uint32_t orphan_tag;              /* etiqueta sem comando esperando */
    uint32_t no_tag;                  /* comando que ficou sem etiqueta */
} ioc_ca_rx;

typedef enum {
    IOC_CA_PASS = 0,   /* não é da autenticação: siga o fluxo normal */
    IOC_CA_HOLD,       /* comando guardado à espera da etiqueta: não execute */
    IOC_CA_RELEASE,    /* etiqueta confere: execute o comando devolvido em `released` */
    IOC_CA_DROP        /* quadro descartado (ver `why`) */
} ioc_ca_action;

typedef enum {
    IOC_CA_WHY_NONE = 0,
    IOC_CA_WHY_BAD_LENGTH,      /* comando que não tem 8 bytes, ou etiqueta que não tem 8 */
    IOC_CA_WHY_BAD_KIND,        /* tipo da etiqueta desconhecido */
    IOC_CA_WHY_ORPHAN_TAG,      /* etiqueta sem comando guardado (ou já vencido) */
    IOC_CA_WHY_BAD_TAG,         /* MAC errado, atualidade que não avança, ou comando trocado */
    IOC_CA_WHY_NO_TAG,          /* o comando guardado venceu sem etiqueta (devolvido por ioc_ca_rx_expire) */
    IOC_CA_WHY_REPLACED         /* um comando novo tomou o lugar de outro ainda sem etiqueta */
} ioc_ca_why;

typedef struct {
    uint16_t id;
    uint8_t  len;
    uint8_t  data[8];
} ioc_ca_frame;

/* `saved` é a atualidade gravada na última vez; 0 para um módulo sem nada gravado. */
void ioc_ca_rx_init(ioc_ca_rx *rx, const uint8_t bus_key[IOC_LINK_KEY_LEN], uint64_t saved);

/* Cada quadro recebido passa por aqui. `why` é preenchido nos descartes (e em REPLACED, mesmo com HOLD). */
ioc_ca_action ioc_ca_rx_frame(ioc_ca_rx *rx, uint32_t now_ms, uint16_t can_id, const uint8_t *data, uint8_t len,
                              ioc_ca_frame *released, ioc_ca_why *why);

/* Chamada periódica: descarta comando sem etiqueta há mais de IOC_CA_TAG_WAIT_MS. Devolve o id do comando
 * descartado (e conta em no_tag), ou 0. */
uint16_t ioc_ca_rx_expire(ioc_ca_rx *rx, uint32_t now_ms);

/* 1 se passaram IOC_CA_RX_SAVE_STEP aceitações desde o valor gravado. */
int ioc_ca_rx_needs_save(const ioc_ca_rx *rx, uint64_t saved);

const char *ioc_ca_why_name(ioc_ca_why why);

#ifdef __cplusplus
}
#endif

#endif
