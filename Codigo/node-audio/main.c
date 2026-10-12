/**
 * @file main.c
 * @brief Sensor acústico INMP441 (Pico W / Pico 2 W): laço principal do núcleo 0.
 *
 * O núcleo 0 cuida de tudo o que conversa com o mundo — enlace com o Node CAN
 * (BLE + Wi-Fi/UDP autenticado), console USB, diagnóstico e bateria — e repassa
 * pedidos de configuração ao núcleo 1, que faz a aquisição e o DSP. Cada
 * relatório do núcleo 1 (um por segundo) vira um datagrama de telemetria, uma
 * linha no console e, se for o caso, um DTC.
 *
 * Vigilância: o núcleo 0 só alimenta o watchdog enquanto o contador de voltas
 * do núcleo 1 avança. Se o DSP travar, o DTC 0x3002 é registrado e o
 * watchdog reinicia a placa.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/watchdog.h"

#include "audio_app.h"
#include "audio_config.h"
#include "audio_console.h"
#include "audio_core1.h"
#include "battery_monitor.h"
#include "edge_diagnostics.h"
#include "edge_network_driver.h"

#define WATCHDOG_TIMEOUT_MS   8000u
#define CORE1_STALL_MS        2000u
#define RESULT_TIMEOUT_MS     3000u

static int8_t   g_env_mcu_temp_c = 0;
static uint16_t g_env_vcc_mv = 0xFFFF;

// Pedido enviado ao núcleo 1 aguardando resultado (um de cada vez por origem).
static bool     g_cfg_in_flight = false;
static uint32_t g_cfg_sent_ms = 0;
static bool     g_ctrl_in_flight = false;
static uint32_t g_ctrl_sent_ms = 0;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static int8_t read_mcu_temp_c(void) {
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    const float voltage = (float)adc_read() * 3.3f / 4096.0f;
    const float temp_c = 27.0f - (voltage - 0.706f) / 0.001721f;
    if (temp_c > 127.0f) return 127;
    if (temp_c < -128.0f) return -128;
    return (int8_t)lroundf(temp_c);
}

static void update_environment(void) {
    g_env_mcu_temp_c = read_mcu_temp_c();
    g_env_vcc_mv = battery_monitor_get_mv();
    // O INMP441 não mede temperatura: o campo do sensor no freeze frame fica em INT8_MIN.
    edge_diag_set_context(audio_app_current_config()->target_mode, INT8_MIN, g_env_mcu_temp_c, g_env_vcc_mv);
}

static void pump_dtcs(void) {
    DTC_Record rec;
    while (edge_diag_pop_pending(&rec)) {
        edge_net_send_urgent_dtc(&rec);
        audio_console_report_dtc(&rec);
    }
}

static uint8_t dtc_code_to_beacon_hint(uint16_t code) {
    if (code == DTC_NONE) return 0x00;
    return (uint8_t)((((code >> 12) & 0x0Fu) << 4) | (code & 0x0Fu));
}

// Pedidos de configuração e de controle seguem para o núcleo 1; o da rede tem
// prioridade sobre o do console, e só um de cada tipo fica em trânsito.
static void forward_requests(void) {
    const uint32_t now = now_ms();
    if (g_cfg_in_flight && now - g_cfg_sent_ms > RESULT_TIMEOUT_MS) g_cfg_in_flight = false;
    if (g_ctrl_in_flight && now - g_ctrl_sent_ms > RESULT_TIMEOUT_MS) g_ctrl_in_flight = false;

    if (!g_cfg_in_flight) {
        AudioCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = AUDIO_CMD_CONFIG;
        if (audio_app_take_pending_config(&cmd.config)) {
            cmd.origin = AUDIO_ORIGIN_NETWORK;
            cmd.token = cmd.config.request_counter;
        } else if (audio_console_take_pending_config(&cmd.config)) {
            cmd.origin = AUDIO_ORIGIN_CONSOLE;
        } else {
            cmd.type = 0;
        }
        if (cmd.type) {
            EdgeConfigValidationError err;
            if (!edge_config_validate(&cmd.config, &err)) {
                // A rede já validou na entrada; o console também. Defesa em profundidade.
                diag_report_dtc_event(DTC_SYS_CONFIG_REJECT, FTB_OUT_OF_RANGE_HIGH, SEV_WARNING);
                if (cmd.origin == AUDIO_ORIGIN_NETWORK) edge_net_send_config_ack(&cmd.config, 2);
            } else if (audio_core1_post(&cmd)) {
                g_cfg_in_flight = true;
                g_cfg_sent_ms = now;
            } else if (cmd.origin == AUDIO_ORIGIN_NETWORK) {
                edge_net_send_config_ack(&cmd.config, 2);   // fila cheia: o Node pode repetir
            }
        }
    }

    if (!g_ctrl_in_flight) {
        AudioPendingControl ctrl;
        AudioCommand cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.type = AUDIO_CMD_CONTROL;
        if (audio_app_take_pending_control(&ctrl)) {
            cmd.origin = AUDIO_ORIGIN_NETWORK;
        } else if (audio_console_take_pending_control(&ctrl)) {
            cmd.origin = AUDIO_ORIGIN_CONSOLE;
        } else {
            return;
        }
        cmd.token = ctrl.request_counter;
        cmd.action = ctrl.action;
        cmd.argument = ctrl.argument;
        if (audio_core1_post(&cmd)) {
            g_ctrl_in_flight = true;
            g_ctrl_sent_ms = now;
        } else if (cmd.origin == AUDIO_ORIGIN_NETWORK) {
            audio_app_send_control_ack(ctrl.request_counter, ctrl.action, 2, 0);
        }
    }
}

static void handle_results(void) {
    AudioCommandResult res;
    while (audio_core1_poll_result(&res)) {
        if (res.type == AUDIO_CMD_CONFIG) {
            g_cfg_in_flight = false;
            if (res.ok) {
                audio_app_set_current_config(&res.config, res.effective_rate_hz);
                update_environment();
            } else {
                diag_report_dtc_event(DTC_SYS_CONFIG_REJECT, FTB_OUT_OF_RANGE_HIGH, SEV_WARNING);
            }
            if (res.origin == AUDIO_ORIGIN_NETWORK) edge_net_send_config_ack(&res.config, res.ok ? 1 : 2);
            audio_console_report_config_result(&res);
        } else {
            g_ctrl_in_flight = false;
            if (res.origin == AUDIO_ORIGIN_NETWORK) {
                audio_app_send_control_ack(res.token, res.action, res.ok ? 1 : 2, 0);
            }
            audio_console_report_control_result(&res);
        }
    }
}

// Condições detectadas pelo núcleo 1 viram DTC aqui (o diagnóstico é do núcleo 0).
static void report_dtcs_from(const AudioReportMessage *msg, uint32_t *lost_seen) {
    const AcousticReport *r = &msg->report;
    if (r->mic_state == ACOUSTIC_MIC_SILENT) {
        diag_report_dtc_event(DTC_I2S_NO_DATA, FTB_SIGNAL_OPEN, SEV_CRITICAL);
    } else if (r->mic_state == ACOUSTIC_MIC_STUCK_HIGH) {
        diag_report_dtc_event(DTC_I2S_NO_DATA, FTB_SIGNAL_STUCK_HIGH, SEV_CRITICAL);
    }
    if (r->flags & ACOUSTIC_FLAG_OVERLOAD) {
        diag_report_dtc_event(DTC_SENS_ACOUSTIC_OVERLOAD, FTB_OUT_OF_RANGE_HIGH, SEV_WARNING);
    }
    if (msg->lost_blocks_total != *lost_seen) {
        *lost_seen = msg->lost_blocks_total;
        diag_report_dtc_event(DTC_DSP_OVERRUN, FTB_SIGNAL_RATE_INVALID, SEV_WARNING);
    }
}

int main(void) {
    stdio_init_all();
    adc_init();
    adc_set_temp_sensor_enabled(true);

    edge_diag_init();
    battery_monitor_init();

    Payload_Configuration initial;
    audio_config_defaults(&initial);
    audio_app_init(&initial);

    // O rádio reserva sua PIO e seus canais de DMA antes do microfone.
    edge_net_init();
    audio_console_init();
    audio_core1_start(&initial);
    update_environment();

    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);

    uint32_t last_beacon_ms = now_ms();
    uint32_t next_beacon_delay_ms = edge_net_next_discovery_delay_ms();
    NetworkMode last_net_state = edge_net_get_state();
    uint32_t last_env_ms = 0;
    uint32_t last_heartbeat = audio_core1_heartbeat();
    uint32_t last_heartbeat_change_ms = now_ms();
    uint32_t lost_seen = 0;
    static AudioReportMessage msg;   // ~1,4 KiB: fora da pilha

    while (true) {
        const uint32_t now = now_ms();

        // Vigilância do núcleo 1.
        const uint32_t heartbeat = audio_core1_heartbeat();
        if (heartbeat != last_heartbeat) {
            last_heartbeat = heartbeat;
            last_heartbeat_change_ms = now;
        }
        const bool core1_alive = now - last_heartbeat_change_ms < CORE1_STALL_MS;
        if (core1_alive) {
            watchdog_update();
        } else {
            diag_report_dtc_event(DTC_DSP_CORE_STALL, FTB_GENERAL_FAILURE, SEV_CRITICAL);
        }

        audio_console_poll();
        battery_monitor_poll();
        pump_dtcs();
        edge_net_poll_timeout();

        const NetworkMode net_state = edge_net_get_state();
        if (net_state != last_net_state) {
            audio_console_report_net_state(net_state);
            last_net_state = net_state;
        }

        forward_requests();
        handle_results();

        while (audio_core1_poll_report(&msg)) {
            report_dtcs_from(&msg, &lost_seen);
            (void)audio_app_send_report(&msg, edge_net_next_seq());
            audio_console_on_report(&msg);
        }

        if (last_env_ms == 0 || now - last_env_ms >= 1000u) {
            update_environment();
            last_env_ms = now;
        }

        // Em DISCOVERY o "beacon" é o HELLO autenticado (edge_network_driver.c).
        if (net_state == NET_STATE_DISCOVERY) {
            if (now - last_beacon_ms >= next_beacon_delay_ms) {
                edge_net_send_discovery_beacon(dtc_code_to_beacon_hint(diagnostics_get_active_code()));
                last_beacon_ms = now;
                next_beacon_delay_ms = edge_net_next_discovery_delay_ms();
            }
        } else {
            last_beacon_ms = now;
            next_beacon_delay_ms = edge_net_next_discovery_delay_ms();
        }

        sleep_ms(2);
    }
}
