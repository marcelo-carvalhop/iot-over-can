/**
 * @file ioc_link.h
 * @brief Autenticação do enlace sensor wireless <-> Node CAN (protocolo v6).
 *
 * Código compartilhado pelos dois firmwares. Não depende de rádio, pilha de
 * rede ou relógio: recebe bytes e devolve bytes, o que permite testá-lo no
 * computador de desenvolvimento.
 *
 * Hierarquia de chaves (todas com 32 bytes):
 *
 *   K_master                       somente Nodes CAN e máquina de provisionamento
 *     ├── K_dev = HMAC(K_master, "ioc-dev-v1" || UUID64)   uma por sensor
 *     └── K_net = HMAC(K_master, "ioc-net-v1")             rede Wi-Fi local
 *           └── PSK do AP do Node N = hex(HMAC(K_net, "ioc-psk-v1" || N))[0..31]
 *
 *   K_sess = HMAC(K_dev, "ioc-sess-v1" || UUID64 || N || nonce_sensor || nonce_node)
 *
 * O Node deriva K_dev de qualquer UUID a partir de K_master. O sensor recebe
 * apenas K_dev e K_net; a extração da memória de um sensor não revela a chave
 * dos demais.
 *
 * Datagrama autenticado ("envelope"), little-endian:
 *
 *   'I' 'L' | versão | tipo | contador(4) | payload(n) | tag(8)
 *
 *   tag = HMAC(chave, cabeçalho || payload)[0..7]
 *
 * HELLO e CHALLENGE usam K_dev; CONFIRM e DATA usam K_sess.
 */
#ifndef IOC_LINK_H
#define IOC_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_LINK_VERSION        0x06
#define IOC_LINK_UDP_PORT       4242
#define IOC_LINK_KEY_LEN        32
#define IOC_LINK_TAG_LEN        8
#define IOC_LINK_NONCE_LEN      8
#define IOC_LINK_SSID_LEN       8   /* "IOC-NN" + NUL, com folga */
#define IOC_LINK_PSK_LEN        33  /* 32 caracteres hexadecimais + NUL */

/* ---------------------------------------------------------------- chaves */

/** Converte 64 caracteres hexadecimais em 32 bytes. Retorna 1 em sucesso. */
int ioc_link_hex_to_key(const char *hex, uint8_t out[IOC_LINK_KEY_LEN]);

/** Retorna 1 quando a chave é composta apenas por zeros (não provisionada). */
int ioc_link_key_is_zero(const uint8_t key[IOC_LINK_KEY_LEN]);

void ioc_link_derive_device_key(const uint8_t master[IOC_LINK_KEY_LEN], uint64_t uuid,
                                uint8_t out[IOC_LINK_KEY_LEN]);
void ioc_link_derive_network_key(const uint8_t master[IOC_LINK_KEY_LEN],
                                 uint8_t out[IOC_LINK_KEY_LEN]);
void ioc_link_derive_session_key(const uint8_t dev_key[IOC_LINK_KEY_LEN], uint64_t uuid,
                                 uint8_t node_id,
                                 const uint8_t sensor_nonce[IOC_LINK_NONCE_LEN],
                                 const uint8_t node_nonce[IOC_LINK_NONCE_LEN],
                                 uint8_t out[IOC_LINK_KEY_LEN]);

/* ----------------------------------------------------------------- Wi-Fi */

void    ioc_link_wifi_ssid(uint8_t node_id, char out[IOC_LINK_SSID_LEN]);
void    ioc_link_wifi_psk(const uint8_t net_key[IOC_LINK_KEY_LEN], uint8_t node_id,
                          char out[IOC_LINK_PSK_LEN]);
/** Canais não sobrepostos de 2,4 GHz distribuídos pelo ID do Node: 1, 6, 11. */
uint8_t ioc_link_wifi_channel(uint8_t node_id);

/* ------------------------------------------------ oferta de vínculo (BLE) */

/*
 * Manufacturer Specific Data anunciado pelo Node responsável:
 *
 *   company(2)=0xFFFF | 'I' 'O' | versão | node | UUID64(8, BE) | canal |
 *   nonce(4, BE) | tag(8)                                   = 27 bytes
 *
 *   tag = HMAC(K_dev, "ioc-offer-v1" || bytes[2..18])[0..7]
 */
#define IOC_OFFER_MFG_LEN 27
#define IOC_OFFER_VERSION 0x01

typedef struct {
    uint64_t uuid;
    uint8_t  node_id;
    uint8_t  channel;
    uint32_t nonce;
} ioc_offer_t;

size_t ioc_offer_build(const uint8_t dev_key[IOC_LINK_KEY_LEN], const ioc_offer_t *offer,
                       uint8_t out[IOC_OFFER_MFG_LEN]);
/** Decodifica sem verificar a tag. Útil para filtrar pelo UUID antes do HMAC. */
int ioc_offer_peek(const uint8_t *mfg, size_t len, ioc_offer_t *out);
/** Decodifica e verifica a tag. Retorna 1 somente quando autêntica. */
int ioc_offer_verify(const uint8_t dev_key[IOC_LINK_KEY_LEN], const uint8_t *mfg, size_t len,
                     ioc_offer_t *out);

/* -------------------------------------------------------------- envelope */

#define IOC_ENV_MAGIC_0    0x49 /* 'I' */
#define IOC_ENV_MAGIC_1    0x4C /* 'L' */
#define IOC_ENV_HEADER_LEN 8
#define IOC_ENV_OVERHEAD   (IOC_ENV_HEADER_LEN + IOC_LINK_TAG_LEN)

typedef enum {
    IOC_MSG_HELLO     = 0x01, /* sensor -> Node, K_dev  */
    IOC_MSG_CHALLENGE = 0x02, /* Node -> sensor, K_dev  */
    IOC_MSG_CONFIRM   = 0x03, /* sensor -> Node, K_sess */
    IOC_MSG_DATA_UP   = 0x10, /* sensor -> Node, K_sess */
    IOC_MSG_DATA_DOWN = 0x11  /* Node -> sensor, K_sess */
} ioc_msg_type;

typedef struct {
    uint8_t        type;
    uint32_t       counter;
    const uint8_t *payload;
    size_t         payload_len;
} ioc_env_view;

/** Monta o datagrama autenticado. Retorna o tamanho total ou 0 se não couber. */
size_t ioc_env_seal(const uint8_t key[IOC_LINK_KEY_LEN], uint8_t type, uint32_t counter,
                    const uint8_t *payload, size_t payload_len,
                    uint8_t *out, size_t out_cap);
/** Valida apenas a estrutura (magic, versão, tamanho). Não autentica. */
int ioc_env_parse(const uint8_t *buf, size_t len, ioc_env_view *view);
/** Verifica a tag do datagrama inteiro. Retorna 1 somente quando autêntico. */
int ioc_env_verify(const uint8_t key[IOC_LINK_KEY_LEN], const uint8_t *buf, size_t len);

/**
 * Janela anti-replay: aceita apenas contadores estritamente crescentes e
 * diferentes de zero. Datagramas UDP fora de ordem são descartados.
 */
int ioc_counter_accept(uint32_t *last_accepted, uint32_t counter);

/* ------------------------------------------------------------- handshake */

#define IOC_HELLO_LEN     18
#define IOC_CHALLENGE_LEN 18
#define IOC_CONFIRM_LEN   16

typedef struct {
    uint64_t uuid;
    uint8_t  profile;
    uint8_t  protocol;
    uint8_t  sensor_nonce[IOC_LINK_NONCE_LEN];
} ioc_hello_t;

typedef struct {
    uint8_t node_id;
    uint8_t child_id;
    uint8_t node_nonce[IOC_LINK_NONCE_LEN];
    uint8_t sensor_nonce[IOC_LINK_NONCE_LEN];
} ioc_challenge_t;

typedef struct {
    uint8_t node_nonce[IOC_LINK_NONCE_LEN];
    uint8_t sensor_nonce[IOC_LINK_NONCE_LEN];
} ioc_confirm_t;

void ioc_hello_encode(const ioc_hello_t *in, uint8_t out[IOC_HELLO_LEN]);
int  ioc_hello_decode(const uint8_t *buf, size_t len, ioc_hello_t *out);
void ioc_challenge_encode(const ioc_challenge_t *in, uint8_t out[IOC_CHALLENGE_LEN]);
int  ioc_challenge_decode(const uint8_t *buf, size_t len, ioc_challenge_t *out);
void ioc_confirm_encode(const ioc_confirm_t *in, uint8_t out[IOC_CONFIRM_LEN]);
int  ioc_confirm_decode(const uint8_t *buf, size_t len, ioc_confirm_t *out);

#ifdef __cplusplus
}
#endif

#endif /* IOC_LINK_H */
