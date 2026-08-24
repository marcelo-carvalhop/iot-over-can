/**
 * @file edge_network_driver.c
 * @brief Implementação de UDP Broadcast para Descoberta e UDP Unicast para Telemetria.
 */
#include "edge_network_driver.h"
#include "battery_monitor.h"
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "lwip/pbuf.h"
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

static NetworkMode     g_net_mode = NET_STATE_DISABLED;
static struct udp_pcb *g_pcb      = NULL;
static bool            g_wifi_enabled = false;
static bool            g_cyw43_initialized = false;
static ip_addr_t       g_gateway_ip;
static u16_t           g_gateway_port = 0;
static uint16_t        g_session_token = 0;
static uint16_t        g_seq_cnt = 0;

// Item 3 (heartbeat/timeout de sessão): timestamp da última atividade válida
// vinda do gateway vinculado (CLAIM, SET_CONFIG ou PING). Se ficar velho
// demais, o nó assume que o gateway sumiu e volta para NET_STATE_DISCOVERY.
static uint32_t        g_last_gateway_activity_ms = 0;

// Item 15: contador de falhas consecutivas de envio UDP, para diagnosticar
// problemas de rede sem inundar o barramento de eventos DTC a cada pacote.
static uint16_t        g_net_tx_consecutive_fail = 0;
#define NET_TX_FAIL_DTC_THRESHOLD 10

extern void diag_report_dtc_event(uint16_t code, uint8_t symptom, uint8_t severity);

static void net_note_tx_result(err_t result) {
    // Em DISCOVERY, falhas de broadcast/sem AP não devem poluir a TUI com DTC
    // de rede. DTC_SYS_NET_TX_FAIL só é relevante depois que há gateway vinculado.
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

static void net_mark_gateway_alive(void) {
    g_last_gateway_activity_ms = to_ms_since_boot(get_absolute_time());
}

// Monta e envia o menu de configuração (valores válidos + estado atual)
// para o gateway atualmente vinculado. Chamado automaticamente logo após
// o claim ser aceito, e também sob demanda via CMD_GET_CONFIG_MENU.
// Deve ser chamada de dentro do contexto lwIP (callback de rx) — não usa
// lock próprio por isso; se precisar ser chamada de fora desse contexto
// no futuro, envolva com cyw43_arch_lwip_begin/end como as demais funções
// públicas deste arquivo.
static void net_send_config_ack_locked_ctx(struct udp_pcb *pcb, const Payload_Configuration *cfg, uint8_t status) {
    if (g_net_mode != NET_STATE_BOUND || !pcb || !cfg) return;

    Payload_ConfigAck ack;
    ack.magic_header = NET_MAGIC_HEADER;
    ack.cmd_type = CMD_ACK_CONFIG;
    ack.session_token = g_session_token;
    ack.status = status;
    ack.target_mode = cfg->target_mode;
    ack.requested_window_size = cfg->window_size;
    ack.effective_window_size = dsp_get_active_window_size();
    ack.requested_sample_rate_hz = cfg->sample_rate_hz;
    ack.effective_sample_rate_hz = mpu6050_get_configured_sample_rate_hz();

    struct pbuf *tx = pbuf_alloc(PBUF_TRANSPORT, sizeof(ack), PBUF_RAM);
    if (tx) {
        memcpy(tx->payload, &ack, sizeof(ack));
        net_note_tx_result(udp_sendto(pcb, tx, &g_gateway_ip, g_gateway_port));
        pbuf_free(tx);
    }
}

static void net_send_dtc_snapshot_locked_ctx(struct udp_pcb *pcb) {
    if (g_net_mode != NET_STATE_BOUND || !pcb) return;

    Payload_DTCSnapshot snap;
    snap.magic_header = NET_MAGIC_HEADER;
    snap.cmd_type = CMD_DTC_SNAPSHOT;
    snap.session_token = g_session_token;
    snap.dtc_count = diagnostics_get_count();
    snap.active_code = bite_get_current_status();

    struct pbuf *tx = pbuf_alloc(PBUF_TRANSPORT, sizeof(snap), PBUF_RAM);
    if (tx) {
        memcpy(tx->payload, &snap, sizeof(snap));
        net_note_tx_result(udp_sendto(pcb, tx, &g_gateway_ip, g_gateway_port));
        pbuf_free(tx);
    }
}

static void net_send_config_menu_locked_ctx(struct udp_pcb *pcb) {
    if (g_net_mode != NET_STATE_BOUND || !pcb) return;

    Payload_ConfigMenu menu;
    menu.magic_header = NET_MAGIC_HEADER;
    menu.cmd_type = CMD_CONFIG_MENU;
    menu.session_token = g_session_token;
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

    struct pbuf *tx = pbuf_alloc(PBUF_TRANSPORT, sizeof(menu), PBUF_RAM);
    if (tx) {
        memcpy(tx->payload, &menu, sizeof(menu));
        net_note_tx_result(udp_sendto(pcb, tx, &g_gateway_ip, g_gateway_port));
        pbuf_free(tx);
    }
}

static void udp_rx_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    if (p == NULL || p->len < 4) {
        if (p) pbuf_free(p);
        return;
    }

    uint8_t *payload = (uint8_t *)p->payload;
    uint16_t magic = (uint16_t)((payload[1] << 8) | payload[0]);
    if (magic != NET_MAGIC_HEADER) {
        pbuf_free(p);
        return;
    }

    uint8_t cmd = payload[2];
    switch (cmd) {
        case CMD_CLAIM_NODE: {
            if (p->len >= sizeof(Payload_ClaimCommand)) {
                Payload_ClaimCommand *claim = (Payload_ClaimCommand *)payload;

                // Item 2 (versão de protocolo) + Item 4 (autenticação leve):
                // rejeita vínculos de versão incompatível ou sem a chave
                // pré-compartilhada correta, em vez de aceitar qualquer
                // CMD_CLAIM_NODE de qualquer host na sub-rede.
                if (claim->protocol_ver != NET_PROTOCOL_VERSION ||
                    claim->auth_key != NODE_PRESHARED_KEY) {
                    diag_report_dtc_event(DTC_SYS_AUTH_REJECT, FTB_GENERAL_FAILURE, SEV_WARNING);
                    break;
                }

                g_session_token = claim->session_token;
                ip_addr_copy(g_gateway_ip, *addr);
                g_gateway_port = port;
                g_net_mode = NET_STATE_BOUND;
                net_mark_gateway_alive();

                Payload_CapabilitiesAck ack;
                ack.magic_header = NET_MAGIC_HEADER;
                ack.cmd_type = CMD_ACK_CAPABILITIES;
                ack.session_token = g_session_token;
                ack.node_uuid = MY_NODE_UUID;
                ack.profile_id = (uint8_t)PROFILE_ACCELEROMETER;
                ack.max_sample_rate = 1000;
                ack.dsp_buffer_max = DSP_BUFFER_MAX_SAMPLES;
                ack.dsp_fw_version = 0x04;

                struct pbuf *tx = pbuf_alloc(PBUF_TRANSPORT, sizeof(ack), PBUF_RAM);
                if (tx) {
                    memcpy(tx->payload, &ack, sizeof(ack));
                    // Callback já roda em contexto do lwIP (invocado via
                    // cyw43_arch_lwip_*), não precisa de lock aqui.
                    net_note_tx_result(udp_sendto(pcb, tx, &g_gateway_ip, g_gateway_port));
                    pbuf_free(tx);
                }

                // Item (menu de configuração): logo após aceitar o vínculo,
                // manda também o menu de valores válidos + config atual —
                // assim o gateway não precisa hardcodar faixas/enums.
                net_send_config_menu_locked_ctx(pcb);
            }
            break;
        }
        case CMD_SET_CONFIG: {
            if (g_net_mode == NET_STATE_BOUND && ip_addr_cmp(addr, &g_gateway_ip)) {
                if (p->len >= sizeof(Payload_Configuration)) {
                    Payload_Configuration *cfg = (Payload_Configuration *)payload;
                    if (cfg->session_token == g_session_token) {
                        net_mark_gateway_alive();
                        dsp_apply_remote_config(cfg);
                        net_send_config_ack_locked_ctx(pcb, cfg, 0); // 0 = queued, aplicação real é confirmada pelo loop principal
                    }
                }
            }
            break;
        }
        case CMD_CLEAR_DTC: {
            if (g_net_mode == NET_STATE_BOUND && ip_addr_cmp(addr, &g_gateway_ip) &&
                p->len >= sizeof(Payload_ClearDTC)) {
                Payload_ClearDTC *clr = (Payload_ClearDTC *)payload;
                if (clr->session_token == g_session_token) {
                    net_mark_gateway_alive();
                    diagnostics_clear_all();
                    net_send_dtc_snapshot_locked_ctx(pcb);
                }
            }
            break;
        }
        case CMD_PING: {
            // Item 3: heartbeat do gateway. Responde com CMD_PONG e renova
            // o timeout de sessão.
            if (g_net_mode == NET_STATE_BOUND && ip_addr_cmp(addr, &g_gateway_ip) &&
                p->len >= sizeof(Payload_Heartbeat)) {
                Payload_Heartbeat *hb = (Payload_Heartbeat *)payload;
                if (hb->session_token == g_session_token) {
                    net_mark_gateway_alive();
                    Payload_Heartbeat pong = {NET_MAGIC_HEADER, CMD_PONG, g_session_token};
                    struct pbuf *tx = pbuf_alloc(PBUF_TRANSPORT, sizeof(pong), PBUF_RAM);
                    if (tx) {
                        memcpy(tx->payload, &pong, sizeof(pong));
                        net_note_tx_result(udp_sendto(pcb, tx, &g_gateway_ip, g_gateway_port));
                        pbuf_free(tx);
                    }
                }
            }
            break;
        }
        case CMD_GET_CONFIG_MENU: {
            // Reenvia o menu sob demanda. Espera-se pelo menos magic+cmd_type
            // (3 bytes); não exige um payload maior porque o pedido não
            // carrega nenhum dado além do comando.
            if (g_net_mode == NET_STATE_BOUND && ip_addr_cmp(addr, &g_gateway_ip)) {
                net_mark_gateway_alive();
                net_send_config_menu_locked_ctx(pcb);
            }
            break;
        }
    }
    pbuf_free(p);
}

static bool edge_net_start_wifi_locked(void) {
    if (g_wifi_enabled && g_pcb) {
        return true;
    }

    ip_addr_t ip, mask, gw;
    IP4_ADDR(&ip, 192, 168, 4, 2);      // IP estático do nó
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 4, 1);      // Soft-AP do ESP32 gateway

    if (!g_cyw43_initialized) {
        if (cyw43_arch_init() != 0) {
            g_net_mode = NET_STATE_DISABLED;
            g_wifi_enabled = false;
            return false;
        }
        g_cyw43_initialized = true;
    }

    cyw43_arch_enable_sta_mode();
    netif_set_addr(netif_default, &ip, &mask, &gw);
    cyw43_arch_wifi_connect_async("CAN_EDGE_GATEWAY_01", "industrial_dsp_secure", CYW43_AUTH_WPA2_AES_PSK);

    if (g_pcb == NULL) {
        g_pcb = udp_new();
        if (g_pcb) {
            udp_bind(g_pcb, IP_ANY_TYPE, NET_UDP_PORT_DISCOVERY);
            udp_recv(g_pcb, udp_rx_callback, NULL);
        }
    }

    if (g_pcb == NULL) {
        g_net_mode = NET_STATE_DISABLED;
        g_wifi_enabled = false;
        return false;
    }

    g_wifi_enabled = true;
    g_net_mode = NET_STATE_DISCOVERY;
    g_session_token = 0;
    g_gateway_port = 0;
    g_seq_cnt = 0;
    g_last_gateway_activity_ms = to_ms_since_boot(get_absolute_time());
    return true;
}

static void edge_net_stop_wifi_locked(void) {
    if (g_pcb) {
        udp_remove(g_pcb);
        g_pcb = NULL;
    }

    g_wifi_enabled = false;
    g_net_mode = NET_STATE_DISABLED;
    g_session_token = 0;
    g_gateway_port = 0;
    g_seq_cnt = 0;
    g_net_tx_consecutive_fail = 0;

    if (g_cyw43_initialized) {
        cyw43_arch_deinit();
        g_cyw43_initialized = false;
    }
}

void edge_net_init(void) {
    // Segurança operacional: Wi-Fi fica desligado no boot. A TUI/operador deve
    // habilitar explicitamente com NET WIFI ON. Isso evita que o nó wireless
    // apareça na rede antes de uma decisão humana.
    g_wifi_enabled = false;
    g_net_mode = NET_STATE_DISABLED;
    g_pcb = NULL;
    g_last_gateway_activity_ms = to_ms_since_boot(get_absolute_time());
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

// Item 3: deve ser chamada periodicamente pelo loop principal. Se estiver
// vinculado e nenhuma atividade do gateway chegou dentro do timeout, o nó
// se "desvincula" e volta a fazer beacon de descoberta — evita ficar
// mandando telemetria para um gateway que já reiniciou/caiu.
void edge_net_poll_timeout(void) {
    if (!g_wifi_enabled || g_net_mode != NET_STATE_BOUND) return;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (now_ms - g_last_gateway_activity_ms > NET_SESSION_TIMEOUT_MS) {
        g_net_mode = NET_STATE_DISCOVERY;
        g_session_token = 0;
    }
}

void edge_net_send_discovery_beacon(uint8_t bite_status) {
    if (!g_wifi_enabled || g_net_mode != NET_STATE_DISCOVERY || !g_pcb) return;
    Payload_Beacon b = {NET_MAGIC_HEADER, CMD_BEACON_BROADCAST, MY_NODE_UUID,
                         (uint8_t)PROFILE_ACCELEROMETER, bite_status,
                         battery_monitor_get_pct_for_beacon(),
                         battery_monitor_get_mv(),
                         (uint8_t)main_get_acquisition_mode(),
                         diagnostics_get_count(),
                         NET_PROTOCOL_VERSION};
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(b), PBUF_RAM);
    if (p) {
        memcpy(p->payload, &b, sizeof(b));
        // Chamada feita a partir do loop principal (fora do contexto do lwIP);
        // com pico_cyw43_arch_lwip_threadsafe_background é obrigatório envolver
        // qualquer chamada lwIP feita fora dos próprios callbacks do lwIP.
        cyw43_arch_lwip_begin();
        err_t r = udp_sendto(g_pcb, p, IP_ADDR_BROADCAST, NET_UDP_PORT_DISCOVERY);
        cyw43_arch_lwip_end();
        net_note_tx_result(r);
        pbuf_free(p);
    }
}

void edge_net_send_telemetry(const DSP_AnalysisResult *res, OperationModeFSM mode, 
                             uint16_t dtc_code, const float *fft_mag, uint16_t fft_bins) {
    if (g_net_mode != NET_STATE_BOUND || !g_pcb) return;

    uint32_t base_sz = sizeof(Payload_TelemetryStream);

    // Item 5: limita quantos bins de FFT são anexados para não ultrapassar
    // NET_MAX_SAFE_PAYLOAD_BYTES e cair em fragmentação IP (custosa e mais
    // sujeita a perda em Wi-Fi). Se o chamador pedir mais bins do que cabe,
    // trunca em vez de estourar o limite silenciosamente.
    uint16_t max_bins_that_fit = (uint16_t)((NET_MAX_SAFE_PAYLOAD_BYTES - base_sz) / sizeof(float));
    uint16_t bins_to_send = (fft_mag != NULL) ? fft_bins : 0;
    if (bins_to_send > max_bins_that_fit) bins_to_send = max_bins_that_fit;

    uint32_t arr_sz = bins_to_send * sizeof(float);
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, base_sz + arr_sz, PBUF_RAM);
    if (p) {
        Payload_TelemetryStream *t = (Payload_TelemetryStream *)p->payload;
        t->magic_header = NET_MAGIC_HEADER;
        t->cmd_type = CMD_TELEMETRY_STREAM;
        t->session_token = g_session_token;
        t->seq_num = g_seq_cnt++;
        t->fsm_mode = (uint8_t)mode;
        t->acquisition_mode = (uint8_t)main_get_acquisition_mode();
        t->axis_mask = res->axis_mask;
        t->fft_valid = res->fft_valid ? 1 : 0;
        t->window_size = res->window_size;
        t->sample_rate_req_hz = main_get_requested_sample_rate_hz();
        t->sample_rate_eff_hz = mpu6050_get_configured_sample_rate_hz();
        t->rms_ac = res->rms_ac;
        t->kurtosis = res->kurtosis;
        t->crest_factor = res->crest_factor;
        t->peak_freq_hz = res->peak_freq_hz;
        t->peak_amplitude = res->peak_amplitude;
        t->entropy = res->spectral_entropy;
        t->ppv_max_mm_s = res->ppv_max_mm_s;
        t->seismic_triggered = res->seismic_triggered ? 1 : 0;
        t->clipping_detected = res->clipping_detected ? 1 : 0;
        t->battery_pct = battery_monitor_get_pct_for_beacon();
        t->battery_mv = battery_monitor_get_mv();
        t->dtc_count = diagnostics_get_count();
        t->dtc_active_code = dtc_code;
        t->fft_bins_count = res->fft_valid ? bins_to_send : 0;
        if (arr_sz > 0) memcpy(((uint8_t *)p->payload) + base_sz, fft_mag, arr_sz);
        cyw43_arch_lwip_begin();
        err_t r = udp_sendto(g_pcb, p, &g_gateway_ip, g_gateway_port);
        cyw43_arch_lwip_end();
        net_note_tx_result(r);
        pbuf_free(p);
    }
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
    if (g_net_mode != NET_STATE_BOUND || !g_pcb) return;

    // Formato: magic(2) + cmd_type(1) + DTC_Record, igual aos demais
    // pacotes do protocolo.
    uint8_t buf[2 + 1 + sizeof(DTC_Record)];
    uint16_t magic = NET_MAGIC_HEADER;
    uint8_t  cmd   = CMD_URGENT_DTC_ALARM;
    memcpy(&buf[0], &magic, 2);
    memcpy(&buf[2], &cmd, 1);
    memcpy(&buf[3], dtc_rec, sizeof(DTC_Record));

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, sizeof(buf), PBUF_RAM);
    if (p) {
        memcpy(p->payload, buf, sizeof(buf));
        cyw43_arch_lwip_begin();
        err_t r = udp_sendto(g_pcb, p, &g_gateway_ip, g_gateway_port);
        cyw43_arch_lwip_end();
        net_note_tx_result(r);
        pbuf_free(p);
    }
}
