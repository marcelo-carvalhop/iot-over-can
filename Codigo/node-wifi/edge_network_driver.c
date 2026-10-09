/**
 * @file edge_network_driver.c
 * @brief Plano de dados do sensor: Wi-Fi/UDP com sessão autenticada (v0x06).
 *
 * Sequência, do ponto de vista do sensor:
 *
 *   1. BLE: o sensor anuncia UUID/perfil (edge_ble_beacon.c) e escuta ofertas.
 *   2. O Node CAN escolhido pelo operador anuncia uma oferta assinada com
 *      K_dev. A oferta informa qual Node assumiu o vínculo.
 *   3. O sensor deriva SSID e senha do ponto de acesso desse Node a partir de
 *      K_net, liga o Wi-Fi e obtém endereço por DHCP.
 *   4. HELLO -> CHALLENGE -> CONFIRM: autenticação mútua por desafio-resposta
 *      e derivação da chave de sessão.
 *   5. Telemetria, espectro e DTCs sobem em DATA_UP; configuração, liveness e
 *      liberação descem em DATA_DOWN. Todo datagrama carrega contador e tag.
 *
 * Nenhum datagrama sem tag válida altera estado. Não existe caminho de
 * compatibilidade com o CLAIM da v0x05.
 */
#include "edge_network_driver.h"
#include "battery_monitor.h"
#include "config_validation.h"
#include "device_identity.h"
#include "edge_ble_beacon.h"
#include "edge_link.h"
#include "ioc_link.h"
#include "ioc_sha256.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "hardware/sync.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "mpu6050_dma_driver.h"

// Definido em main.c — segue o mesmo padrão já usado para
// diag_report_dtc_event(): um extern simples em vez de criar um módulo
// "system_state" só para isso.
extern OperationModeFSM main_get_active_fsm_mode(void);
extern AcquisitionMode main_get_acquisition_mode(void);
extern uint8_t diagnostics_get_count(void);
extern void diagnostics_clear_all(void);
extern uint16_t bite_get_current_status(void);
extern float main_get_requested_sample_rate_hz(void);
extern void diag_report_dtc_event(uint16_t code, uint8_t symptom, uint8_t severity);

static NetworkMode     g_net_mode = NET_STATE_DISABLED;
static struct udp_pcb *g_pcb      = NULL;
static bool            g_wifi_enabled = false;
static bool            g_cyw43_initialized = false;
static ip_addr_t       g_parent_node_ip;
static u16_t           g_parent_node_port = 0;
static uint16_t        g_seq_cnt = 0;

#define EDGE_WIFI_SSID_MAX_LEN 32
#define EDGE_WIFI_PASSWORD_MAX_LEN 63
static char            g_wifi_ssid[EDGE_WIFI_SSID_MAX_LEN + 1] = {0};
static char            g_wifi_password[EDGE_WIFI_PASSWORD_MAX_LEN + 1] = {0};
static bool            g_wifi_provisioned = false;
static uint32_t        g_beacon_prng_state = 0x6D2B79F5u;

// ---------------------------------------------------------------------------
// Estado do enlace autenticado
// ---------------------------------------------------------------------------
static uint8_t  g_session_key[IOC_LINK_KEY_LEN];
static uint8_t  g_sensor_nonce[IOC_LINK_NONCE_LEN];
static bool     g_hello_outstanding = false; // há um HELLO aguardando CHALLENGE
static uint32_t g_tx_counter = 0;            // último contador enviado na sessão
static uint32_t g_rx_last_counter = 0;       // último contador aceito do Node
static uint8_t  g_parent_node_id = 0;        // Node informado pela oferta/CHALLENGE
static uint8_t  g_child_id = 0;              // filho lógico atribuído pelo Node
static bool     g_joined_by_offer = false;   // Wi-Fi ligado por oferta (não por comando de bancada)
static uint32_t g_discovery_since_ms = 0;
static uint32_t g_last_offer_ms = 0;
static uint32_t g_last_rejoin_ms = 0;
static uint32_t g_link_down_since_ms = 0;    // 0 = enlace Wi-Fi ativo ou ainda não observado
static volatile bool g_release_requested = false; // CMD_RELEASE recebido; tratado no laço principal

// Oferta BLE autenticada mais recente. Escrita no contexto da pilha
// Bluetooth, consumida pelo laço principal.
static volatile bool    g_offer_pending = false;
static volatile uint8_t g_offer_node_id = 0;

static uint16_t        g_auth_reject_count = 0;
static uint16_t        g_protocol_reject_count = 0;
static uint16_t        g_replay_reject_count = 0;
static uint16_t        g_config_reject_count = 0;

// Item 3 (liveness lease/timeout de sessão): timestamp da última atividade válida
// vinda do nó CAN pai vinculado. Se ficar velho demais, o nó assume que o nó
// CAN pai sumiu e volta para NET_STATE_DISCOVERY. Escrito pelo callback de
// recepção (contexto assíncrono) e lido pelo laço principal.
static volatile uint32_t g_last_parent_activity_ms = 0;

// Item 15: contador de falhas consecutivas de envio UDP, para diagnosticar
// problemas de rede sem inundar o barramento de eventos DTC a cada pacote.
static uint16_t        g_net_tx_consecutive_fail = 0;
#define NET_TX_FAIL_DTC_THRESHOLD 10

// Maior payload interno que cabe em um datagrama sem fragmentação IP.
#define NET_MAX_INNER_BYTES (NET_MAX_SAFE_PAYLOAD_BYTES - IOC_ENV_OVERHEAD)
static uint8_t g_tx_inner[NET_MAX_INNER_BYTES];
static uint8_t g_rx_datagram[256]; // comandos do Node são curtos; o maior tem ~50 bytes

// ---------------------------------------------------------------------------
// Diagnóstico no console USB
// ---------------------------------------------------------------------------
// Uma linha "[NET] ..." por mudança no caminho até a sessão com o Node:
// oferta recebida, estado do Wi-Fi, endereço obtido, HELLO enviado,
// datagramas recebidos, recusas e sessão. Tudo é impresso pelo laço
// principal, nunca pelos callbacks do lwIP ou da pilha Bluetooth, e só
// quando algo muda, para não inundar o console.
#define DIAG_WIFI_UNSEEN  0x7FFF
#define DIAG_OFFER_REPEAT_MS 10000u
#define DIAG_RX_REPEAT_MS     5000u
static int         g_diag_wifi = DIAG_WIFI_UNSEEN;
static NetworkMode g_diag_mode = NET_STATE_DISABLED;
static uint16_t    g_diag_auth = 0;
static uint16_t    g_diag_protocol = 0;
static uint16_t    g_diag_replay = 0;
static uint8_t     g_diag_offer_node = 0;
static int         g_diag_offer_decision = -1;
static uint32_t    g_diag_offer_ms = 0;
static uint32_t    g_diag_hello_count = 0;
static volatile uint32_t g_diag_rx_count = 0;   // incrementado no callback de recepção
static uint32_t    g_diag_rx_printed = 0;
static uint32_t    g_diag_rx_ms = 0;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static void saturating_inc_u16(uint16_t *value) {
    if (value && *value < UINT16_MAX) (*value)++;
}

static uint32_t beacon_prng_next(void) {
    uint32_t x = g_beacon_prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_beacon_prng_state = x ? x : 0x6D2B79F5u;
    return g_beacon_prng_state;
}

static size_t bounded_strlen(const char *text, size_t max_len) {
    size_t n = 0;
    if (!text) return 0;
    while (n < max_len && text[n] != '\0') n++;
    return n;
}

static void net_note_tx_result(err_t result) {
    // Em DISCOVERY, falhas de envio sem AP não devem poluir a TUI com DTC de
    // rede. DTC_SYS_NET_TX_FAIL só é relevante depois que há sessão.
    if (g_net_mode != NET_STATE_BOUND) {
        g_net_tx_consecutive_fail = 0;
        return;
    }

    if (result == ERR_OK) {
        g_net_tx_consecutive_fail = 0;
        return;
    }
    if (g_net_tx_consecutive_fail < 0xFFFF) g_net_tx_consecutive_fail++;
    if (g_net_tx_consecutive_fail == NET_TX_FAIL_DTC_THRESHOLD) {
        diag_report_dtc_event(DTC_SYS_NET_TX_FAIL, FTB_GENERAL_FAILURE, SEV_WARNING);
    }
}

static void net_mark_parent_alive(void) {
    g_last_parent_activity_ms = now_ms();
}

static void link_reset_session(void) {
    ioc_secure_zero(g_session_key, sizeof(g_session_key));
    g_tx_counter = 0;
    g_rx_last_counter = 0;
    g_hello_outstanding = false;
    g_parent_node_port = 0;
    g_child_id = 0;
    g_seq_cnt = 0;
}

// Sela `inner` no envelope e envia. Deve ser chamada dentro do contexto lwIP
// (callback de recepção ou entre cyw43_arch_lwip_begin/end).
static err_t link_send_locked_ctx(struct udp_pcb *pcb, const uint8_t *key, uint8_t type,
                                  uint32_t counter, const void *inner, size_t inner_len,
                                  const ip_addr_t *dst, u16_t port) {
    if (!pcb || !dst || inner_len > NET_MAX_INNER_BYTES) return ERR_ARG;
    const size_t total = inner_len + IOC_ENV_OVERHEAD;
    struct pbuf *tx = pbuf_alloc(PBUF_TRANSPORT, (u16_t)total, PBUF_RAM);
    if (!tx) return ERR_MEM;
    err_t result = ERR_BUF;
    if (ioc_env_seal(key, type, counter, (const uint8_t *)inner, inner_len,
                     (uint8_t *)tx->payload, total) == total) {
        result = udp_sendto(pcb, tx, dst, port);
    }
    pbuf_free(tx);
    return result;
}

// Envia um payload interno ao Node pai dentro da sessão autenticada.
static void link_send_data_locked_ctx(struct udp_pcb *pcb, const void *inner, size_t inner_len) {
    if (g_net_mode != NET_STATE_BOUND || !pcb) return;
    if (g_tx_counter == UINT32_MAX) {
        // Contador esgotado: força nova sessão em vez de reutilizar valores.
        g_net_mode = NET_STATE_DISCOVERY;
        link_reset_session();
        return;
    }
    net_note_tx_result(link_send_locked_ctx(pcb, g_session_key, IOC_MSG_DATA_UP, ++g_tx_counter,
                                            inner, inner_len, &g_parent_node_ip, g_parent_node_port));
}

static void net_send_config_ack_locked_ctx(struct udp_pcb *pcb, const Payload_Configuration *cfg, uint8_t status) {
    if (g_net_mode != NET_STATE_BOUND || !pcb || !cfg) return;

    Payload_ConfigAck ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic_header = NET_MAGIC_HEADER;
    ack.cmd_type = CMD_ACK_CONFIG;
    ack.request_counter = cfg->request_counter;
    ack.status = status;
    ack.target_mode = cfg->target_mode;
    ack.requested_window_size = cfg->window_size;
    ack.effective_window_size = dsp_get_active_window_size();
    ack.requested_sample_rate_hz = cfg->sample_rate_hz;
    ack.effective_sample_rate_hz = mpu6050_get_configured_sample_rate_hz();
    link_send_data_locked_ctx(pcb, &ack, sizeof(ack));
}

static void net_send_dtc_snapshot_locked_ctx(struct udp_pcb *pcb) {
    if (g_net_mode != NET_STATE_BOUND || !pcb) return;

    Payload_DTCSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.magic_header = NET_MAGIC_HEADER;
    snap.cmd_type = CMD_DTC_SNAPSHOT;
    snap.dtc_count = diagnostics_get_count();
    snap.active_code = bite_get_current_status();
    snap.auth_reject_count = g_auth_reject_count;
    snap.protocol_reject_count = g_protocol_reject_count;
    snap.replay_reject_count = g_replay_reject_count;
    snap.config_reject_count = g_config_reject_count;
    link_send_data_locked_ctx(pcb, &snap, sizeof(snap));
}

// Descreve os valores válidos de configuração e o estado atual. Enviado logo
// após a sessão ser estabelecida e sob demanda (CMD_GET_CONFIG_MENU).
static void net_send_config_menu_locked_ctx(struct udp_pcb *pcb) {
    if (g_net_mode != NET_STATE_BOUND || !pcb) return;

    Payload_ConfigMenu menu;
    memset(&menu, 0, sizeof(menu));
    menu.magic_header = NET_MAGIC_HEADER;
    menu.cmd_type = CMD_CONFIG_MENU;
    menu.num_fsm_modes = FSM_MODE_SEISMIC_STALTA + 1;   // 0..3
    menu.num_window_types = WIN_BLACKMAN_H + 1;          // 0..4
    menu.min_sample_rate_hz = 4;
    menu.max_sample_rate_hz = 1000;
    menu.min_buffer_size = DSP_BUFFER_MIN_SAMPLES;
    menu.max_buffer_size = DSP_BUFFER_MAX_SAMPLES;
    menu.current_buffer_size = dsp_get_active_window_size();
    menu.current_fsm_mode = (uint8_t)main_get_active_fsm_mode();
    menu.current_window_type = (uint8_t)dsp_get_current_window();
    menu.current_sample_rate_hz = mpu6050_get_configured_sample_rate_hz();
    menu.current_stalta_thresh = dsp_get_stalta_threshold();
    menu.current_calib_gain = dsp_get_calibration_gain();
    link_send_data_locked_ctx(pcb, &menu, sizeof(menu));
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------
static void handle_challenge(struct udp_pcb *pcb, const uint8_t *datagram, size_t len,
                             const ioc_env_view *view, const ip_addr_t *addr, u16_t port) {
    ioc_challenge_t challenge;

    // A tag é conferida antes de qualquer decisão: um CHALLENGE forjado não
    // pode derrubar nem sequestrar a sessão.
    if (!ioc_env_verify(edge_link_device_key(), datagram, len)) {
        saturating_inc_u16(&g_auth_reject_count);
        diag_report_dtc_event(DTC_SYS_AUTH_REJECT, FTB_GENERAL_FAILURE, SEV_WARNING);
        return;
    }
    if (!ioc_challenge_decode(view->payload, view->payload_len, &challenge)) {
        saturating_inc_u16(&g_protocol_reject_count);
        return;
    }
    // O eco do nonce prova que o CHALLENGE responde ao HELLO mais recente;
    // um CHALLENGE autêntico porém antigo (repetição) é descartado aqui.
    if (!g_hello_outstanding) {
        // Resposta duplicada a um HELLO já atendido: descarta sem alarme.
        saturating_inc_u16(&g_replay_reject_count);
        return;
    }
    if (!ioc_ct_equal(challenge.sensor_nonce, g_sensor_nonce, IOC_LINK_NONCE_LEN)) {
        saturating_inc_u16(&g_replay_reject_count);
        diag_report_dtc_event(DTC_SYS_REPLAY_REJECT, FTB_GENERAL_FAILURE, SEV_WARNING);
        return;
    }
    // Quando o Wi-Fi foi ligado por oferta, só o Node da oferta pode abrir sessão.
    if (g_joined_by_offer && g_parent_node_id != 0 && challenge.node_id != g_parent_node_id) {
        saturating_inc_u16(&g_auth_reject_count);
        return;
    }

    ioc_link_derive_session_key(edge_link_device_key(), edge_device_uuid64(), challenge.node_id,
                                g_sensor_nonce, challenge.node_nonce, g_session_key);
    g_hello_outstanding = false;
    g_parent_node_id = challenge.node_id;
    g_child_id = challenge.child_id;
    g_tx_counter = 0;
    g_rx_last_counter = 0;
    g_seq_cnt = 0;
    ip_addr_copy(g_parent_node_ip, *addr);
    g_parent_node_port = port;
    g_net_mode = NET_STATE_BOUND;
    net_mark_parent_alive();

    ioc_confirm_t confirm;
    uint8_t raw[IOC_CONFIRM_LEN];
    memcpy(confirm.node_nonce, challenge.node_nonce, IOC_LINK_NONCE_LEN);
    memcpy(confirm.sensor_nonce, g_sensor_nonce, IOC_LINK_NONCE_LEN);
    ioc_confirm_encode(&confirm, raw);
    net_note_tx_result(link_send_locked_ctx(pcb, g_session_key, IOC_MSG_CONFIRM, ++g_tx_counter,
                                            raw, sizeof(raw), &g_parent_node_ip, g_parent_node_port));
    net_send_config_menu_locked_ctx(pcb);
}

// ---------------------------------------------------------------------------
// Comandos do Node dentro da sessão autenticada
// ---------------------------------------------------------------------------
static void handle_session_command(struct udp_pcb *pcb, const uint8_t *inner, size_t inner_len) {
    if (inner_len < 3) return;
    uint16_t magic = (uint16_t)((inner[1] << 8) | inner[0]);
    if (magic != NET_MAGIC_HEADER) {
        saturating_inc_u16(&g_protocol_reject_count);
        return;
    }

    switch (inner[2]) {
        case CMD_SET_CONFIG: {
            Payload_Configuration cfg;
            if (inner_len != sizeof(cfg)) break;
            memcpy(&cfg, inner, sizeof(cfg));

            EdgeConfigValidationError validation_error;
            if (!edge_config_validate(&cfg, &validation_error)) {
                (void)validation_error;
                saturating_inc_u16(&g_config_reject_count);
                diag_report_dtc_event(DTC_SYS_CONFIG_REJECT, FTB_OUT_OF_RANGE_HIGH, SEV_WARNING);
                net_send_config_ack_locked_ctx(pcb, &cfg, 2);
                break;
            }
            dsp_apply_remote_config(&cfg);
            net_send_config_ack_locked_ctx(pcb, &cfg, 0);
            break;
        }

        case CMD_CLEAR_DTC:
            if (inner_len != sizeof(Payload_ClearDTC)) break;
            diagnostics_clear_all();
            net_send_dtc_snapshot_locked_ctx(pcb);
            break;

        case CMD_PING: {
            Payload_Heartbeat hb;
            if (inner_len != sizeof(hb)) break;
            memcpy(&hb, inner, sizeof(hb));
            Payload_Heartbeat pong;
            memset(&pong, 0, sizeof(pong));
            pong.magic_header = NET_MAGIC_HEADER;
            pong.cmd_type = CMD_PONG;
            pong.request_counter = hb.request_counter;
            link_send_data_locked_ctx(pcb, &pong, sizeof(pong));
            break;
        }

        case CMD_GET_CONFIG_MENU:
            net_send_config_menu_locked_ctx(pcb);
            break;

        case CMD_RELEASE:
            // O Wi-Fi não pode ser desligado de dentro do callback do lwIP;
            // o laço principal conclui a liberação.
            g_release_requested = true;
            break;

        default:
            saturating_inc_u16(&g_protocol_reject_count);
            break;
    }
}

static void udp_rx_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg;
    if (p == NULL) return;
    g_diag_rx_count++;

    // Tamanho inesperado é descartado antes de qualquer interpretação.
    if (p->tot_len < IOC_ENV_OVERHEAD || p->tot_len > sizeof(g_rx_datagram)) {
        pbuf_free(p);
        return;
    }
    const size_t len = p->tot_len;
    if (pbuf_copy_partial(p, g_rx_datagram, (u16_t)len, 0) != len) {
        pbuf_free(p);
        return;
    }
    pbuf_free(p);

    ioc_env_view view;
    if (!edge_link_is_provisioned() || !ioc_env_parse(g_rx_datagram, len, &view)) {
        // Inclui datagramas da v0x05 e qualquer tráfego estranho à rede local.
        saturating_inc_u16(&g_protocol_reject_count);
        return;
    }

    if (view.type == IOC_MSG_CHALLENGE) {
        // Aceito em DISCOVERY e também em BOUND: se o Node reiniciou, ele
        // responde ao próximo HELLO e a sessão antiga é substituída.
        handle_challenge(pcb, g_rx_datagram, len, &view, addr, port);
        return;
    }

    if (view.type != IOC_MSG_DATA_DOWN || g_net_mode != NET_STATE_BOUND) return;

    if (!ioc_env_verify(g_session_key, g_rx_datagram, len)) {
        saturating_inc_u16(&g_auth_reject_count);
        diag_report_dtc_event(DTC_SYS_AUTH_REJECT, FTB_GENERAL_FAILURE, SEV_WARNING);
        return;
    }
    if (!ioc_counter_accept(&g_rx_last_counter, view.counter)) {
        saturating_inc_u16(&g_replay_reject_count);
        diag_report_dtc_event(DTC_SYS_REPLAY_REJECT, FTB_GENERAL_FAILURE, SEV_WARNING);
        return;
    }

    // Endereço de origem é apenas roteamento; a identidade vem da tag. Se o
    // Node mudou de porta efêmera, as respostas passam a seguir o novo destino.
    ip_addr_copy(g_parent_node_ip, *addr);
    g_parent_node_port = port;
    net_mark_parent_alive();
    handle_session_command(pcb, view.payload, view.payload_len);
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------
static bool edge_net_start_wifi_locked(void) {
    if (g_wifi_enabled && g_pcb) return true;
    if (!g_wifi_provisioned || g_wifi_ssid[0] == '\0') return false;

    if (!g_cyw43_initialized) {
        if (cyw43_arch_init() != 0) {
            g_net_mode = NET_STATE_DISABLED;
            g_wifi_enabled = false;
            return false;
        }
        g_cyw43_initialized = true;
    }

    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_async(g_wifi_ssid, g_wifi_password, CYW43_AUTH_WPA2_AES_PSK) != 0) {
        g_net_mode = NET_STATE_DISABLED;
        g_wifi_enabled = false;
        return false;
    }

    /* Dynamic address: supports multiple sensors under the same CAN node/AP. */
    cyw43_arch_lwip_begin();
    if (netif_default) dhcp_start(netif_default);
    if (g_pcb == NULL) {
        g_pcb = udp_new();
        if (g_pcb) {
            udp_bind(g_pcb, IP_ANY_TYPE, NET_UDP_PORT_DISCOVERY);
            udp_recv(g_pcb, udp_rx_callback, NULL);
        }
    }
    cyw43_arch_lwip_end();
    if (g_pcb == NULL) {
        g_net_mode = NET_STATE_DISABLED;
        g_wifi_enabled = false;
        return false;
    }

    g_wifi_enabled = true;
    g_net_mode = NET_STATE_DISCOVERY;
    link_reset_session();
    g_last_parent_activity_ms = now_ms();
    g_discovery_since_ms = g_last_parent_activity_ms;
    g_last_rejoin_ms = g_last_parent_activity_ms;
    return true;
}

static void edge_net_stop_wifi_locked(void) {
    if (g_cyw43_initialized) {
        cyw43_arch_lwip_begin();
        if (g_pcb) {
            udp_remove(g_pcb);
            g_pcb = NULL;
        }
        if (netif_default) dhcp_stop(netif_default);
        cyw43_arch_lwip_end();
    } else {
        g_pcb = NULL;
    }

    g_wifi_enabled = false;
    g_net_mode = NET_STATE_DISABLED;
    link_reset_session();
    g_net_tx_consecutive_fail = 0;

    if (g_cyw43_initialized) {
        // Não desinicializa o CYW43: o advertising e a varredura BLE continuam
        // ativos com o Wi-Fi desligado. Apenas a interface STA é desligada aqui.
        cyw43_arch_disable_sta_mode();
    }
}

// Chamado no contexto da pilha Bluetooth para cada Manufacturer Data visto.
static void on_ble_manufacturer_data(const uint8_t *mfg, size_t len) {
    ioc_offer_t offer;
    if (!edge_link_is_provisioned()) return;
    if (!ioc_offer_peek(mfg, len, &offer)) return;       // não é oferta
    if (offer.uuid != edge_device_uuid64()) return;      // oferta para outro sensor
    if (!ioc_offer_verify(edge_link_device_key(), mfg, len, &offer)) {
        saturating_inc_u16(&g_auth_reject_count);        // UUID nosso, tag inválida
        return;
    }
    g_offer_node_id = offer.node_id;
    g_offer_pending = true;
}

void edge_net_init(void) {
    edge_device_identity_init();
    edge_link_init(edge_device_uuid64());
    g_wifi_enabled = false;
    g_wifi_provisioned = false;
    g_net_mode = NET_STATE_DISABLED;
    g_pcb = NULL;
    g_last_parent_activity_ms = now_ms();
    g_beacon_prng_state ^= (uint32_t)edge_device_uuid64();
    g_beacon_prng_state ^= (uint32_t)(edge_device_uuid64() >> 32);
    link_reset_session();

    /* BLE discovery is autonomous at boot. Wi-Fi remains disabled until an
       authenticated offer names the CAN node that took responsibility for
       this sensor. pico_btstack_cyw43 makes cyw43_arch_init() initialize the
       shared Bluetooth/Wi-Fi radio. */
    if (cyw43_arch_init() == 0) {
        g_cyw43_initialized = true;
        edge_ble_offer_scan_set_handler(on_ble_manufacturer_data);
        (void)edge_ble_beacon_init();
        if (edge_link_is_provisioned()) edge_ble_offer_scan_enable(true);
    }
}

bool edge_net_provision_wifi(const char *ssid, const char *password) {
    if (!ssid || !password) return false;
    size_t ssid_len = bounded_strlen(ssid, EDGE_WIFI_SSID_MAX_LEN + 1);
    size_t pass_len = bounded_strlen(password, EDGE_WIFI_PASSWORD_MAX_LEN + 1);
    if (ssid_len == 0 || ssid_len > EDGE_WIFI_SSID_MAX_LEN ||
        pass_len < 8 || pass_len > EDGE_WIFI_PASSWORD_MAX_LEN) return false;
    memcpy(g_wifi_ssid, ssid, ssid_len);
    g_wifi_ssid[ssid_len] = '\0';
    memcpy(g_wifi_password, password, pass_len);
    g_wifi_password[pass_len] = '\0';
    g_wifi_provisioned = true;
    return true;
}

void edge_net_clear_wifi_provisioning(void) {
    edge_net_stop_wifi_locked();
    ioc_secure_zero(g_wifi_ssid, sizeof(g_wifi_ssid));
    ioc_secure_zero(g_wifi_password, sizeof(g_wifi_password));
    g_wifi_provisioned = false;
    g_joined_by_offer = false;
    g_parent_node_id = 0;
}

bool edge_net_is_wifi_provisioned(void) {
    return g_wifi_provisioned;
}

uint32_t edge_net_next_discovery_delay_ms(void) {
    /* 1.5..3.5 s randomized backoff between HELLO attempts. */
    return 1500u + (beacon_prng_next() % 2001u);
}

bool edge_net_set_enabled(bool enabled) {
    if (enabled) {
        return edge_net_start_wifi_locked();
    }
    edge_net_stop_wifi_locked();
    return true;
}

bool edge_net_is_enabled(void) {
    return g_wifi_enabled;
}

NetworkMode edge_net_get_state(void) { return g_net_mode; }

uint8_t edge_net_parent_node_id(void) { return g_parent_node_id; }
uint8_t edge_net_child_id(void) { return g_child_id; }
uint16_t edge_net_auth_reject_count(void) { return g_auth_reject_count; }
uint16_t edge_net_replay_reject_count(void) { return g_replay_reject_count; }

static const char *diag_wifi_name(int status) {
    switch (status) {
        case CYW43_LINK_DOWN:    return "desligado";
        case CYW43_LINK_JOIN:    return "associando ao ponto de acesso";
        case CYW43_LINK_NOIP:    return "associado, aguardando endereco (DHCP)";
        case CYW43_LINK_UP:      return "conectado";
        case CYW43_LINK_FAIL:    return "falha na conexao";
        case CYW43_LINK_NONET:   return "rede nao encontrada";
        case CYW43_LINK_BADAUTH: return "senha recusada";
        default:                 return "estado desconhecido";
    }
}

// Decisão tomada diante de uma oferta: 0 ignorada (sessão ativa), 1 aceita,
// 2 aguardando (tentativa com outro Node ainda dentro do prazo).
static void diag_offer(uint8_t node, int decision, uint32_t now) {
    if (node == g_diag_offer_node && decision == g_diag_offer_decision &&
        now - g_diag_offer_ms < DIAG_OFFER_REPEAT_MS) return;
    g_diag_offer_node = node;
    g_diag_offer_decision = decision;
    g_diag_offer_ms = now;
    switch (decision) {
        case 1:
            printf("[NET] oferta do Node %u conferida: entrando no Wi-Fi IOC-%02u\n", (unsigned)node, (unsigned)node);
            break;
        case 2:
            printf("[NET] oferta do Node %u conferida: aguardando, tentativa com o Node %u em andamento\n",
                   (unsigned)node, (unsigned)g_parent_node_id);
            break;
        default:
            printf("[NET] oferta do Node %u ignorada: sessao ativa com o Node %u\n",
                   (unsigned)node, (unsigned)g_parent_node_id);
            break;
    }
}

static void diag_poll(uint32_t now) {
    if (g_wifi_enabled) {
        const int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (status != g_diag_wifi) {
            g_diag_wifi = status;
            if (status == CYW43_LINK_UP && netif_default) {
                char ip[IP4ADDR_STRLEN_MAX];
                char gw[IP4ADDR_STRLEN_MAX];
                cyw43_arch_lwip_begin();
                ip4addr_ntoa_r(netif_ip4_addr(netif_default), ip, sizeof(ip));
                ip4addr_ntoa_r(netif_ip4_gw(netif_default), gw, sizeof(gw));
                cyw43_arch_lwip_end();
                printf("[NET] Wi-Fi %s: conectado, ip=%s gateway=%s\n", g_wifi_ssid, ip, gw);
            } else {
                printf("[NET] Wi-Fi %s: %s (%d)\n", g_wifi_ssid, diag_wifi_name(status), status);
            }
        }
    } else if (g_diag_wifi != DIAG_WIFI_UNSEEN) {
        g_diag_wifi = DIAG_WIFI_UNSEEN;
        printf("[NET] Wi-Fi desligado\n");
    }

    if (g_net_mode != g_diag_mode) {
        if (g_net_mode == NET_STATE_BOUND) {
            printf("[NET] sessao autenticada com o Node %u, filho %u\n", (unsigned)g_parent_node_id,
                   (unsigned)g_child_id);
        } else if (g_diag_mode == NET_STATE_BOUND) {
            printf("[NET] sessao encerrada\n");
        }
        g_diag_mode = g_net_mode;
        g_diag_hello_count = 0;
    }

    if (g_auth_reject_count != g_diag_auth || g_protocol_reject_count != g_diag_protocol ||
        g_replay_reject_count != g_diag_replay) {
        g_diag_auth = g_auth_reject_count;
        g_diag_protocol = g_protocol_reject_count;
        g_diag_replay = g_replay_reject_count;
        printf("[NET] recusados: autenticacao=%u protocolo=%u repeticao=%u\n", (unsigned)g_diag_auth,
               (unsigned)g_diag_protocol, (unsigned)g_diag_replay);
    }

    const uint32_t rx = g_diag_rx_count;
    if (rx != g_diag_rx_printed && g_net_mode != NET_STATE_BOUND &&
        (g_diag_rx_printed == 0 || now - g_diag_rx_ms >= DIAG_RX_REPEAT_MS)) {
        printf("[NET] datagramas recebidos do Node: %lu\n", (unsigned long)rx);
        g_diag_rx_printed = rx;
        g_diag_rx_ms = now;
    }
}

// Entra no ponto de acesso do Node indicado por uma oferta autenticada.
// `keep_discovery_timer` preserva o início da tentativa quando a associação
// ao mesmo Node está apenas sendo refeita, para que o limite de desistência
// continue valendo.
static void join_node_access_point(uint8_t node_id, bool keep_discovery_timer) {
    char ssid[IOC_LINK_SSID_LEN];
    char psk[IOC_LINK_PSK_LEN];
    const uint32_t discovery_since = g_discovery_since_ms;

    if (g_wifi_enabled) edge_net_stop_wifi_locked();
    ioc_link_wifi_ssid(node_id, ssid);
    ioc_link_wifi_psk(edge_link_network_key(), node_id, psk);
    bool ok = edge_net_provision_wifi(ssid, psk);
    ioc_secure_zero(psk, sizeof(psk));
    if (!ok) return;

    g_parent_node_id = node_id;
    g_joined_by_offer = true;
    (void)edge_net_start_wifi_locked();
    if (keep_discovery_timer) g_discovery_since_ms = discovery_since;
    g_link_down_since_ms = 0;
}

// Deve ser chamada a cada iteração do laço principal. Trata a oferta BLE
// pendente, novas tentativas de associação ao AP e os tempos limite de sessão.
void edge_net_poll_timeout(void) {
    const uint32_t now = now_ms();

    if (g_release_requested) {
        g_release_requested = false;
        edge_net_clear_wifi_provisioning();
        g_last_offer_ms = 0;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    bool offer_pending = g_offer_pending;
    uint8_t offer_node = g_offer_node_id;
    g_offer_pending = false;
    restore_interrupts(irq_state);

    if (offer_pending) {
        g_last_offer_ms = now;
        // Com sessão ativa, ofertas são ignoradas: quem decide a troca de
        // responsável é o operador (UNBIND seguido de BIND).
        if (g_net_mode != NET_STATE_BOUND) {
            const bool idle = !g_wifi_enabled || !g_joined_by_offer;
            // Uma oferta de outro Node só é seguida depois de dar tempo à
            // tentativa atual. Sem isso, uma oferta antiga repetida por um
            // terceiro faria o rádio alternar de AP indefinidamente.
            const bool may_switch = offer_node != g_parent_node_id &&
                                    now - g_discovery_since_ms > NET_OFFER_SWITCH_HOLD_MS;
            if (idle || may_switch) {
                diag_offer(offer_node, 1, now);
                join_node_access_point(offer_node, false);
            } else if (offer_node != g_parent_node_id) {
                diag_offer(offer_node, 2, now);
            }
        } else {
            diag_offer(offer_node, 0, now);
        }
    }

    // A última atividade é lida antes do relógio: se o callback de recepção
    // atualizá-la entre as duas leituras, a diferença continua não negativa.
    const uint32_t last_activity = g_last_parent_activity_ms;
    const uint32_t now_after = now_ms();
    if (g_wifi_enabled && g_net_mode == NET_STATE_BOUND &&
        (int32_t)(now_after - last_activity) > (int32_t)NET_SESSION_TIMEOUT_MS) {
        cyw43_arch_lwip_begin();
        g_net_mode = NET_STATE_DISCOVERY;
        link_reset_session();
        cyw43_arch_lwip_end();
        g_discovery_since_ms = now_after;
        g_link_down_since_ms = 0;
    }

    if (g_wifi_enabled && g_net_mode == NET_STATE_DISCOVERY) {
        int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link == CYW43_LINK_UP) {
            g_link_down_since_ms = 0;
        } else if (g_link_down_since_ms == 0) {
            g_link_down_since_ms = now ? now : 1u;
        }

        if (link < 0 && now - g_last_rejoin_ms > NET_WIFI_REJOIN_MS) {
            // CYW43_LINK_FAIL / NONET / BADAUTH: o rádio desistiu; tenta de novo.
            g_last_rejoin_ms = now;
            (void)cyw43_arch_wifi_connect_async(g_wifi_ssid, g_wifi_password, CYW43_AUTH_WPA2_AES_PSK);
        } else if (link != CYW43_LINK_UP && g_link_down_since_ms != 0 &&
                   now - g_link_down_since_ms > NET_WIFI_LINK_DOWN_MS &&
                   now - g_last_rejoin_ms > NET_WIFI_REJOIN_MS) {
            // Depois de perder um enlace já estabelecido o driver permanece em
            // DOWN/JOIN, sem código de erro. A associação é refeita por inteiro.
            g_last_rejoin_ms = now;
            if (g_joined_by_offer && g_parent_node_id != 0) {
                join_node_access_point(g_parent_node_id, true);
                g_last_rejoin_ms = now;
            } else {
                (void)cyw43_arch_wifi_connect_async(g_wifi_ssid, g_wifi_password, CYW43_AUTH_WPA2_AES_PSK);
                g_link_down_since_ms = now ? now : 1u;
            }
        }

        if (g_wifi_enabled && g_joined_by_offer &&
            now - g_discovery_since_ms > NET_DISCOVERY_GIVEUP_MS &&
            now - g_last_offer_ms > NET_DISCOVERY_GIVEUP_MS / 3u) {
            // O Node deixou de ofertar e não respondeu: volta ao estado de espera.
            edge_net_clear_wifi_provisioning();
        }
    }

    // A varredura BLE só é necessária enquanto não há sessão.
    if (g_cyw43_initialized && edge_link_is_provisioned()) {
        bool want_scan = (g_net_mode != NET_STATE_BOUND);
        if (want_scan != edge_ble_offer_scan_is_active()) edge_ble_offer_scan_enable(want_scan);
    }

    diag_poll(now_ms());
}

// Mantém o nome da v0x05 para não alterar main.c: em DISCOVERY, o "beacon"
// passou a ser o HELLO autenticado que inicia o desafio-resposta.
void edge_net_send_discovery_beacon(uint8_t bite_status) {
    (void)bite_status;
    if (!g_wifi_enabled || g_net_mode != NET_STATE_DISCOVERY || !g_pcb) return;
    if (!edge_link_is_provisioned()) return;
    if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) return;

    ioc_hello_t hello;
    uint8_t raw[IOC_HELLO_LEN];
    uint8_t nonce[IOC_LINK_NONCE_LEN];
    uint64_t r = get_rand_64();
    memcpy(nonce, &r, IOC_LINK_NONCE_LEN);
    hello.uuid = edge_device_uuid64();
    hello.profile = (uint8_t)PROFILE_ACCELEROMETER;
    hello.protocol = NET_PROTOCOL_VERSION;
    memcpy(hello.sensor_nonce, nonce, IOC_LINK_NONCE_LEN);
    ioc_hello_encode(&hello, raw);

    // Chamada feita a partir do loop principal (fora do contexto do lwIP);
    // com pico_cyw43_arch_lwip_threadsafe_background é obrigatório envolver
    // qualquer chamada lwIP feita fora dos próprios callbacks do lwIP. O
    // nonce em uso só é trocado com o lock adquirido, e o estado é conferido
    // de novo: um CHALLENGE pode ter sido aceito enquanto o HELLO era montado.
    cyw43_arch_lwip_begin();
    if (g_net_mode != NET_STATE_DISCOVERY || !g_pcb) {
        cyw43_arch_lwip_end();
        return;
    }
    memcpy(g_sensor_nonce, nonce, IOC_LINK_NONCE_LEN);
    g_hello_outstanding = true;
    ip_addr_t dst;
    const ip4_addr_t *gw = netif_default ? netif_ip4_gw(netif_default) : NULL;
    if (gw && !ip4_addr_isany(gw)) {
        ip_addr_copy_from_ip4(dst, *gw);       // o Node é o gateway da rede do AP
    } else {
        ip_addr_copy(dst, *IP_ADDR_BROADCAST);
    }
    err_t result = link_send_locked_ctx(g_pcb, edge_link_device_key(), IOC_MSG_HELLO, 0,
                                        raw, sizeof(raw), &dst, NET_UDP_PORT_DISCOVERY);
    cyw43_arch_lwip_end();
    net_note_tx_result(result);

    // As três primeiras tentativas e depois uma a cada dez.
    g_diag_hello_count++;
    if (g_diag_hello_count <= 3u || g_diag_hello_count % 10u == 0u) {
        char addr[IPADDR_STRLEN_MAX];
        ipaddr_ntoa_r(&dst, addr, sizeof(addr));
        printf("[NET] HELLO %lu enviado ao Node em %s:%u (%s)\n", (unsigned long)g_diag_hello_count, addr,
               (unsigned)NET_UDP_PORT_DISCOVERY, result == ERR_OK ? "ok" : "erro de envio");
    }
}

void edge_net_send_telemetry(const DSP_AnalysisResult *res, OperationModeFSM mode,
                             uint16_t dtc_code, const float *fft_mag, uint16_t fft_bins) {
    if (g_net_mode != NET_STATE_BOUND || !g_pcb || !res) return;

    const uint32_t base_sz = sizeof(Payload_TelemetryStream);

    // Item 5: limita quantos bins de FFT são anexados para não ultrapassar
    // NET_MAX_SAFE_PAYLOAD_BYTES (já descontado o envelope) e cair em
    // fragmentação IP. Se o chamador pedir mais do que cabe, trunca.
    uint16_t max_bins_that_fit = (uint16_t)((NET_MAX_INNER_BYTES - base_sz) / sizeof(float));
    uint16_t bins_to_send = (fft_mag != NULL && res->fft_valid) ? fft_bins : 0;
    if (bins_to_send > max_bins_that_fit) bins_to_send = max_bins_that_fit;
    const uint32_t arr_sz = (uint32_t)bins_to_send * sizeof(float);

    Payload_TelemetryStream t;
    memset(&t, 0, sizeof(t));
    t.magic_header = NET_MAGIC_HEADER;
    t.cmd_type = CMD_TELEMETRY_STREAM;
    t.seq_num = g_seq_cnt++;
    t.fsm_mode = (uint8_t)mode;
    t.acquisition_mode = (uint8_t)main_get_acquisition_mode();
    t.axis_mask = res->axis_mask;
    t.fft_valid = res->fft_valid ? 1 : 0;
    t.window_size = res->window_size;
    t.sample_rate_req_hz = main_get_requested_sample_rate_hz();
    t.sample_rate_eff_hz = mpu6050_get_configured_sample_rate_hz();
    t.rms_ac = res->rms_ac;
    t.kurtosis = res->kurtosis;
    t.crest_factor = res->crest_factor;
    t.peak_freq_hz = res->peak_freq_hz;
    t.peak_amplitude = res->peak_amplitude;
    t.entropy = res->spectral_entropy;
    t.ppv_max_mm_s = res->ppv_max_mm_s;
    t.seismic_triggered = res->seismic_triggered ? 1 : 0;
    t.clipping_detected = res->clipping_detected ? 1 : 0;
    t.battery_pct = battery_monitor_get_pct_for_beacon();
    t.battery_mv = battery_monitor_get_mv();
    t.dtc_count = diagnostics_get_count();
    t.dtc_active_code = dtc_code;
    t.fft_bins_count = bins_to_send;

    // g_tx_inner é usado apenas pelo laço principal; os envios feitos de
    // dentro do callback de recepção usam estruturas próprias na pilha.
    memcpy(g_tx_inner, &t, base_sz);
    if (arr_sz > 0) memcpy(&g_tx_inner[base_sz], fft_mag, arr_sz);

    cyw43_arch_lwip_begin();
    link_send_data_locked_ctx(g_pcb, g_tx_inner, base_sz + arr_sz);
    cyw43_arch_lwip_end();
}


void edge_net_send_config_ack(const Payload_Configuration *cfg, uint8_t status) {
    if (g_net_mode != NET_STATE_BOUND || !g_pcb || !cfg) return;
    cyw43_arch_lwip_begin();
    net_send_config_ack_locked_ctx(g_pcb, cfg, status);
    cyw43_arch_lwip_end();
}

void edge_net_send_dtc_snapshot(void) {
    if (g_net_mode != NET_STATE_BOUND || !g_pcb) return;
    cyw43_arch_lwip_begin();
    net_send_dtc_snapshot_locked_ctx(g_pcb);
    cyw43_arch_lwip_end();
}

// IMPORTANTE: esta função NUNCA deve ser chamada diretamente de uma ISR
// (ex.: dma_complete_isr). Chamadas lwIP fora do contexto do próprio lwIP
// precisam do lock cyw43_arch_lwip_begin/end, que por sua vez não é seguro
// para uso dentro de uma interrupção. Use diag_report_dtc_event() normalmente
// (ela apenas enfileira) e chame diag_pump_pending_dtc() no loop principal,
// que é quem efetivamente invoca esta função.
void edge_net_send_urgent_dtc(const DTC_Record *dtc_rec) {
    if (g_net_mode != NET_STATE_BOUND || !g_pcb || !dtc_rec) return;

    // Formato interno: magic(2) + cmd_type(1) + DTC_Record, igual aos demais
    // pacotes do protocolo.
    uint8_t buf[2 + 1 + sizeof(DTC_Record)];
    uint16_t magic = NET_MAGIC_HEADER;
    uint8_t  cmd   = CMD_URGENT_DTC_ALARM;
    memcpy(&buf[0], &magic, 2);
    memcpy(&buf[2], &cmd, 1);
    memcpy(&buf[3], dtc_rec, sizeof(DTC_Record));

    cyw43_arch_lwip_begin();
    link_send_data_locked_ctx(g_pcb, buf, sizeof(buf));
    cyw43_arch_lwip_end();
}
