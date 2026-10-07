/**
 * @file ioc_wdata.h
 * @brief Plano de dados wireless sobre CAN clássico: segmentação e formatos.
 *
 * Um Node CAN publica os dados dos sensores sob sua responsabilidade em um
 * identificador próprio (CAN_ID_WIRELESS_DATA_BASE + NODE_ID). Mensagens
 * maiores que um quadro são divididas em segmentos de 8 bytes:
 *
 *   byte 0   filho(3 bits) | tipo(5 bits)
 *   byte 1   transferência(3 bits) | índice do segmento(5 bits)
 *   índice 0:  byte 2 = tamanho total, byte 3 = CRC-8, bytes 4..7 = dados
 *   índice >0: bytes 2..7 = dados
 *
 * Capacidade: 4 + 31 * 6 = 190 bytes por transferência. Os segmentos de uma
 * transferência saem do mesmo Node, no mesmo identificador, e portanto
 * chegam em ordem; uma lacuna aborta a remontagem.
 *
 * Comandos no sentido inverso cabem em um único quadro
 * (CAN_ID_WIRELESS_DATA_CMD):
 *
 *   seq | node | filho | opcode | arg0 | arg1 | arg2 | arg3
 */
#ifndef IOC_WDATA_H
#define IOC_WDATA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_WD_FRAME_LEN        8
#define IOC_WD_MAX_SEGMENTS     32
#define IOC_WD_MAX_PAYLOAD      190
#define IOC_WD_MAX_CHILD        7
#define IOC_WD_RX_SLOTS         2
#define IOC_WD_RX_TIMEOUT_MS    1500u

typedef enum {
    IOC_WD_KIND_TELEMETRY = 1,
    IOC_WD_KIND_SPECTRUM  = 2,
    IOC_WD_KIND_CMD_ACK   = 3,
    IOC_WD_KIND_DTC       = 4,
    IOC_WD_KIND_LINK      = 5,
    IOC_WD_KIND_CONFIG    = 6
} ioc_wd_kind;

uint8_t ioc_wd_crc8(const uint8_t *data, size_t len);

/** Quantidade de quadros necessária para len bytes; 0 quando não cabe. */
uint8_t ioc_wd_segment_count(size_t len);

/** Preenche o quadro de índice `index`. Retorna 1 em sucesso. */
int ioc_wd_build_segment(uint8_t child, uint8_t kind, uint8_t transfer,
                         const uint8_t *payload, size_t len, uint8_t index,
                         uint8_t frame[IOC_WD_FRAME_LEN]);

typedef struct {
    uint8_t  active;
    uint8_t  child;
    uint8_t  kind;
    uint8_t  transfer;
    uint8_t  total_len;
    uint8_t  crc;
    uint8_t  next_index;
    uint8_t  received;
    uint32_t started_ms;
    uint8_t  data[IOC_WD_MAX_PAYLOAD];
} ioc_wd_rx_slot;

typedef struct {
    ioc_wd_rx_slot slots[IOC_WD_RX_SLOTS];
} ioc_wd_rx;

typedef enum {
    IOC_WD_RX_PENDING  = 0,
    IOC_WD_RX_COMPLETE = 1,
    IOC_WD_RX_ERROR    = -1
} ioc_wd_rx_result;

void ioc_wd_rx_reset(ioc_wd_rx *rx);
/**
 * Entrega um quadro ao remontador. Quando retorna IOC_WD_RX_COMPLETE, *done
 * aponta para o slot com a mensagem íntegra (CRC conferido), válido até a
 * próxima chamada.
 */
ioc_wd_rx_result ioc_wd_rx_push(ioc_wd_rx *rx, const uint8_t frame[IOC_WD_FRAME_LEN],
                                uint32_t now_ms, const ioc_wd_rx_slot **done);

/* ------------------------------------------------------------- telemetria */

#define IOC_WD_TEL_FLAG_FFT_VALID 0x01
#define IOC_WD_TEL_FLAG_CLIPPING  0x02
#define IOC_WD_TEL_FLAG_STALTA    0x04
#define IOC_WD_TELEMETRY_LEN      50

typedef struct {
    uint16_t seq;
    uint8_t  mode;
    uint8_t  acquisition;
    uint8_t  axis_mask;
    uint8_t  flags;
    uint16_t window_size;
    float    rate_req_hz;
    float    rate_eff_hz;
    float    rms;
    float    kurtosis;
    float    crest;
    float    peak_hz;
    float    peak_amp;
    float    entropy;
    float    ppv_mm_s;
    uint8_t  battery_pct;
    uint16_t battery_mv;
    uint8_t  dtc_count;
    uint16_t dtc_code;
} ioc_wd_telemetry;

size_t ioc_wd_telemetry_encode(const ioc_wd_telemetry *in, uint8_t out[IOC_WD_TELEMETRY_LEN]);
int    ioc_wd_telemetry_decode(const uint8_t *buf, size_t len, ioc_wd_telemetry *out);

/* --------------------------------------------------------------- espectro */

#define IOC_WD_SPECTRUM_HEADER_LEN 17
#define IOC_WD_SPECTRUM_CHUNK_BINS 80
#define IOC_WD_SPECTRUM_MAX_BINS   256

typedef struct {
    uint8_t  spectrum_id;
    uint16_t total_bins;
    uint16_t offset;
    uint8_t  count;
    float    scale;          /* magnitude correspondente a 65535 */
    float    sample_rate_hz;
    uint16_t fft_size;
    uint8_t  window_type;
} ioc_wd_spectrum_header;

/** Codifica cabeçalho + `count` magnitudes normalizadas (u16 LE). */
size_t ioc_wd_spectrum_encode(const ioc_wd_spectrum_header *hdr, const uint16_t *bins,
                              uint8_t *out, size_t out_cap);
int    ioc_wd_spectrum_decode(const uint8_t *buf, size_t len, ioc_wd_spectrum_header *hdr,
                              const uint8_t **bins_le);

/**
 * Reduz `in_bins` magnitudes para `out_bins` pelo máximo de cada faixa e
 * normaliza para u16. Devolve a escala (maior magnitude) em *scale.
 */
void ioc_wd_spectrum_reduce(const float *in, uint16_t in_bins, uint16_t *out, uint16_t out_bins,
                            float *scale);

/* ------------------------------------------------- demais mensagens curtas */

#define IOC_WD_ACK_LEN    5
#define IOC_WD_DTC_LEN    8
#define IOC_WD_LINK_LEN   15
#define IOC_WD_CONFIG_LEN 21

typedef enum {
    IOC_WD_ACK_ACCEPTED = 0, /* entregue ao sensor, aguardando efeito */
    IOC_WD_ACK_APPLIED  = 1,
    IOC_WD_ACK_REJECTED = 2,
    IOC_WD_ACK_NO_LINK  = 3, /* sem sessão autenticada com o sensor */
    IOC_WD_ACK_UNKNOWN  = 4  /* filho ou opcode desconhecido */
} ioc_wd_ack_status;

typedef struct {
    uint8_t  seq;
    uint8_t  opcode;
    uint8_t  status;
    uint16_t detail;
} ioc_wd_ack;

typedef struct {
    uint16_t code;
    uint8_t  symptom;
    uint8_t  severity;
    uint32_t timestamp_ms;
} ioc_wd_dtc;

typedef enum {
    IOC_WD_LINK_DOWN      = 0, /* vínculo lógico sem canal de dados */
    IOC_WD_LINK_OFFERING  = 1, /* Node anunciando a oferta BLE */
    IOC_WD_LINK_HANDSHAKE = 2, /* sensor entrou no AP; autenticação em curso */
    IOC_WD_LINK_SECURE    = 3, /* sessão autenticada ativa */
    IOC_WD_LINK_NO_KEY    = 4  /* Node sem chave de enlace provisionada */
} ioc_wd_link_state;

typedef struct {
    uint8_t  state;
    int8_t   wifi_rssi;
    uint16_t session_age_s;
    uint16_t rx_datagrams;
    uint16_t auth_failures;
    uint16_t replay_drops;
    uint16_t lost_datagrams;
    uint8_t  stream_enabled;   /* telemetria sendo encaminhada ao CAN */
    uint16_t stream_period_ms;
} ioc_wd_link;

typedef struct {
    uint8_t  mode;
    uint8_t  window_type;
    uint16_t window_size;
    float    rate_req_hz;
    float    rate_eff_hz;
    float    stalta;
    float    gain;
    uint8_t  status; /* 0=informativo, 1=aplicada, 2=rejeitada */
} ioc_wd_config;

size_t ioc_wd_ack_encode(const ioc_wd_ack *in, uint8_t out[IOC_WD_ACK_LEN]);
int    ioc_wd_ack_decode(const uint8_t *buf, size_t len, ioc_wd_ack *out);
size_t ioc_wd_dtc_encode(const ioc_wd_dtc *in, uint8_t out[IOC_WD_DTC_LEN]);
int    ioc_wd_dtc_decode(const uint8_t *buf, size_t len, ioc_wd_dtc *out);
size_t ioc_wd_link_encode(const ioc_wd_link *in, uint8_t out[IOC_WD_LINK_LEN]);
int    ioc_wd_link_decode(const uint8_t *buf, size_t len, ioc_wd_link *out);
size_t ioc_wd_config_encode(const ioc_wd_config *in, uint8_t out[IOC_WD_CONFIG_LEN]);
int    ioc_wd_config_decode(const uint8_t *buf, size_t len, ioc_wd_config *out);

/* ---------------------------------------------------------------- comandos */

typedef enum {
    IOC_WD_OP_STREAM       = 0x01, /* arg0: 0=off 1=on 2=uma amostra; arg1..2: período ms */
    IOC_WD_OP_FFT          = 0x02, /* arg0..1: quantidade de faixas */
    IOC_WD_OP_STATUS       = 0x03, /* publica LINK + CONFIG */
    IOC_WD_OP_SET_MODE     = 0x10, /* arg0: modo */
    IOC_WD_OP_SET_RATE     = 0x11, /* arg0..3: float32 Hz */
    IOC_WD_OP_SET_WINDOW   = 0x12, /* arg0: tipo (0xFF mantém); arg1..2: tamanho (0 mantém) */
    IOC_WD_OP_SET_STALTA   = 0x13, /* arg0..3: float32 */
    IOC_WD_OP_SET_GAIN     = 0x14, /* arg0..3: float32 */
    IOC_WD_OP_APPLY        = 0x1F, /* envia a configuração preparada ao sensor */
    IOC_WD_OP_DTC_CLEAR    = 0x20
} ioc_wd_opcode;

typedef struct {
    uint8_t seq;
    uint8_t node_id;
    uint8_t child_id;
    uint8_t opcode;
    uint8_t arg[4];
} ioc_wd_command;

void ioc_wd_command_encode(const ioc_wd_command *in, uint8_t frame[IOC_WD_FRAME_LEN]);
void ioc_wd_command_decode(const uint8_t frame[IOC_WD_FRAME_LEN], ioc_wd_command *out);

uint16_t ioc_wd_arg_u16(const uint8_t *arg);
float    ioc_wd_arg_f32(const uint8_t arg[4]);
void     ioc_wd_put_arg_u16(uint8_t *arg, uint16_t value);
void     ioc_wd_put_arg_f32(uint8_t arg[4], float value);

#ifdef __cplusplus
}
#endif

#endif /* IOC_WDATA_H */
