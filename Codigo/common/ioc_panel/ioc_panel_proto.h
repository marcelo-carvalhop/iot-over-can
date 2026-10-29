/**
 * @file ioc_panel_proto.h
 * @brief Painel em papel eletrônico (perfil 0x04): formatos do enlace e do CAN.
 *
 * O painel é um filho wireless como os sensores: anuncia o perfil no BLE,
 * recebe a oferta assinada, autentica-se por HELLO/CHALLENGE/CONFIRM e troca
 * DATA_UP/DATA_DOWN com o Node. Dentro desses datagramas, como nas demais
 * mensagens do sensor, vêm magic (0xAA55, LE) e o tipo do comando:
 *
 *   0x30 FRAMES    Node -> painel  quadros CAN observados pelo Node (tap)
 *   0x31 HELLO     painel -> Node  pede o retrato e declara o intervalo de sono
 *   0x32 STATUS    painel -> Node  bateria, ambiente, página, sono, desligamento
 *   0x33 MARK      painel -> Node  marca do operador (botão, nota, roteiro)
 *   0x34 MARK_ACK  Node -> painel  marca entregue ao CAN
 *
 * O contrato entre Node e painel é o próprio barramento: FRAMES leva quadros
 * CAN, inclusive os do retrato (último quadro de cada chave de estado) e da
 * caixa postal (eventos ocorridos enquanto o painel dormia). O painel usa um
 * único decodificador, qualquer que seja a fonte dos quadros.
 *
 * No CAN, STATUS e MARK sobem como a transferência do tipo 8 do Node que
 * atende o painel: tipo(1) + corpo, o mesmo corpo do DATA_UP.
 */
#ifndef IOC_PANEL_PROTO_H
#define IOC_PANEL_PROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_PANEL_PROFILE        0x04
#define IOC_PANEL_VERSION        1
#define IOC_PANEL_MAGIC          0xAA55u

#define IOC_PANEL_CMD_FRAMES     0x30
#define IOC_PANEL_CMD_HELLO      0x31
#define IOC_PANEL_CMD_STATUS     0x32
#define IOC_PANEL_CMD_MARK       0x33
#define IOC_PANEL_CMD_MARK_ACK   0x34

/* Tipo da transferência CAN que leva STATUS e MARK (ver ioc_wdata.h). */
#define IOC_PANEL_WD_KIND        8

/* ----------------------------------------------------------------- FRAMES */

#define IOC_PANEL_FR_SNAPSHOT    0x01  /* retrato: último quadro de cada chave */
#define IOC_PANEL_FR_MAILBOX     0x02  /* eventos acumulados durante o sono */
#define IOC_PANEL_FR_LIVE        0x04  /* tráfego em tempo real */
#define IOC_PANEL_FR_END         0x08  /* último datagrama do retrato e da caixa postal */

#define IOC_PANEL_FRAMES_HEADER  13
#define IOC_PANEL_FRAME_MAX      13     /* idade(2) + id(2) + dlc(1) + 8 */
#define IOC_PANEL_DATAGRAM_MAX   1200   /* abaixo do MTU com o envelope */

typedef struct {
    uint8_t  flags;
    uint16_t seq;           /* contador de lotes do Node */
    uint16_t span_ms;       /* tempo do Node coberto pelos quadros ao vivo */
    uint32_t skipped_bits;  /* bits de quadros contados mas não encaminhados (carga, espectro) */
    uint8_t  count;
} ioc_panel_frames_hdr;

typedef struct {
    uint32_t age_ms;   /* idade no Node quando o lote saiu */
    uint16_t id;       /* identificador de 11 bits */
    uint8_t  own;      /* transmitido pelo próprio Node */
    uint8_t  len;
    uint8_t  data[8];
} ioc_panel_frame;

/* Idade em 16 bits: até 32 767 ms em milissegundos; acima, em segundos. */
uint16_t ioc_panel_age_encode(uint32_t ms);
uint32_t ioc_panel_age_decode(uint16_t raw);

/* Escreve magic, comando e cabeçalho; devolve o tamanho usado ou 0. */
size_t ioc_panel_frames_begin(uint8_t *out, size_t cap, const ioc_panel_frames_hdr *hdr);
/* Acrescenta um quadro; devolve o novo tamanho ou 0 se não couber. */
size_t ioc_panel_frames_add(uint8_t *out, size_t cap, size_t used, const ioc_panel_frame *frame);
/* Atualiza flags do lote já montado (por exemplo, END no último). */
void   ioc_panel_frames_set_flags(uint8_t *out, uint8_t flags);
uint8_t ioc_panel_frames_count(const uint8_t *out);

/* Lê o cabeçalho (o buffer começa no magic); *offset aponta o primeiro quadro. */
int ioc_panel_frames_parse(const uint8_t *buf, size_t len, ioc_panel_frames_hdr *hdr, size_t *offset);
/* Lê o próximo quadro; devolve 1, 0 no fim ou -1 em formato inválido. */
int ioc_panel_frames_next(const uint8_t *buf, size_t len, size_t *offset, ioc_panel_frame *frame);

/* ------------------------------------------------------------------ HELLO */

#define IOC_PANEL_HELLO_LEN      7
#define IOC_PANEL_HELLO_SNAPSHOT 0x01

typedef struct {
    uint8_t  version;
    uint8_t  flags;
    uint16_t sleep_s;    /* 0 = acordado sempre (alimentado) */
} ioc_panel_hello;

size_t ioc_panel_hello_encode(const ioc_panel_hello *in, uint8_t out[IOC_PANEL_HELLO_LEN]);
int    ioc_panel_hello_decode(const uint8_t *buf, size_t len, ioc_panel_hello *out);

/* ----------------------------------------------------------------- STATUS */

#define IOC_PANEL_ST_USB         0x01  /* alimentado pela USB */
#define IOC_PANEL_ST_SLEEP       0x02  /* vai dormir por sleep_s */
#define IOC_PANEL_ST_POWER_OFF   0x04  /* desligado pelo operador */
#define IOC_PANEL_ST_CLOCK       0x08  /* relógio ajustado */
#define IOC_PANEL_ST_KEYBOARD    0x10  /* teclado CardKB presente */
#define IOC_PANEL_ST_ALERT       0x20  /* alerta anunciado ainda sem ciência do operador */

#define IOC_PANEL_STATUS_BODY    23
#define IOC_PANEL_NA_I16         ((int16_t)-32768)
#define IOC_PANEL_NA_U16         0xFFFFu

typedef struct {
    uint8_t  flags;
    uint8_t  battery_pct;   /* 255 = sem medida */
    uint16_t battery_mv;
    int16_t  temp_c10;      /* IOC_PANEL_NA_I16 = sem medida */
    uint16_t rh_10;         /* IOC_PANEL_NA_U16 = sem medida */
    uint16_t sleep_s;
    uint32_t page_hash;
    uint16_t page_seq;
    uint16_t refreshes;
    uint32_t uptime_s;
    uint8_t  severity;      /* 0 normal, 1 a rede está tratando, 2 atenção, 3 requer você */
} ioc_panel_status;

size_t ioc_panel_status_encode_body(const ioc_panel_status *in, uint8_t out[IOC_PANEL_STATUS_BODY]);
int    ioc_panel_status_decode_body(const uint8_t *buf, size_t len, ioc_panel_status *out);

/* ------------------------------------------------------------------- MARK */

#define IOC_PANEL_MARK_TEXT_MAX  96
#define IOC_PANEL_MARK_FIXED     9      /* seq, kind, step, age, text_len */

typedef enum {
    IOC_PANEL_MARK_BUTTON  = 1,  /* intervenção física, sem texto */
    IOC_PANEL_MARK_NOTE    = 2,  /* nota digitada no teclado */
    IOC_PANEL_MARK_STEP    = 3,  /* passo de roteiro de bancada */
    IOC_PANEL_MARK_RESULT  = 4,  /* resultado medido de um passo */
    IOC_PANEL_MARK_FLAG    = 5,  /* marca do operador pelo teclado (Enter), sem texto */
    IOC_PANEL_MARK_ACK     = 6   /* operador ciente de um alerta; step = nível reconhecido */
} ioc_panel_mark_kind;

typedef struct {
    uint16_t seq;
    uint8_t  kind;
    uint8_t  step;
    uint32_t age_ms;            /* tempo desde a marca até o envio */
    uint8_t  text_len;
    char     text[IOC_PANEL_MARK_TEXT_MAX + 1];
} ioc_panel_mark;

size_t ioc_panel_mark_encode_body(const ioc_panel_mark *in, uint8_t *out, size_t cap);
int    ioc_panel_mark_decode_body(const uint8_t *buf, size_t len, ioc_panel_mark *out);

/* ------------------------------------------------------- mensagens inteiras */

/* magic + comando + corpo, prontos para DATA_UP/DATA_DOWN. */
size_t ioc_panel_msg(uint8_t cmd, const uint8_t *body, size_t body_len, uint8_t *out, size_t cap);
/* Devolve o comando (0 se não for mensagem do painel) e o corpo. */
uint8_t ioc_panel_msg_parse(const uint8_t *buf, size_t len, const uint8_t **body, size_t *body_len);

/* Corpo do MARK_ACK: seq(2). */
#define IOC_PANEL_MARK_ACK_BODY  2

/* Nomes usados nas linhas da Probe. */
const char *ioc_panel_mark_kind_name(uint8_t kind);

#ifdef __cplusplus
}
#endif

#endif
