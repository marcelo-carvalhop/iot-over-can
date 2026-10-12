/**
 * @file audio_console.c
 * @brief Console ASCII via USB CDC do sensor acústico.
 *
 * Protocolo igual ao do sensor de vibração (serial_console.c): uma linha por
 * comando, eco local, respostas OK/ERR e linhas CHAVE=VALOR. Os campos de TEL
 * são os mesmos que a Probe 00 imprime para um sensor acústico ligado a um
 * Node CAN, para que a TUI use um único decodificador.
 */
#include "audio_console.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include "audio_config.h"
#include "battery_monitor.h"
#include "device_identity.h"
#include "edge_ble_beacon.h"
#include "edge_diagnostics.h"
#include "edge_link.h"
#include "inmp441_i2s.h"

#ifndef EDGE_SERIAL_ADMIN_TOKEN
#define EDGE_SERIAL_ADMIN_TOKEN 0ULL
#endif
#ifndef EDGE_BOARD_NAME
#define EDGE_BOARD_NAME "pico_w"
#endif

#define LINE_BUF_SIZE 160
#define SERIAL_AUTH_LEASE_MS 300000u
#define RESET_CONFIRM_WINDOW_MS 10000u
#define BOOT_WELCOME_TIMEOUT_MS 1500u
#define CAL_HISTORY 3

static char     g_linebuf[LINE_BUF_SIZE];
static uint16_t g_linelen = 0;
static bool     g_telemetry_stream = false;
static bool     g_telemetry_once = false;
static bool     g_fft_once = false;
static uint32_t g_telemetry_period_ms = 1000;
static uint32_t g_last_telemetry_ms = 0;
static uint32_t g_serial_auth_until_ms = 0;
static uint32_t g_reset_confirm_until_ms = 0;

static Payload_Configuration g_staged;
static bool                  g_pending_cfg_valid = false;
static Payload_Configuration g_pending_cfg;
static AudioPendingControl   g_pending_ctrl;

// Último relatório (STATUS, CAL) e histórico do LZeq para a calibração.
static AudioReportMessage g_last;
static bool               g_have_last = false;
static float              g_cal_lzeq[CAL_HISTORY];
static unsigned           g_cal_count = 0;
static uint8_t            g_last_mic_state = ACOUSTIC_MIC_OK;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static bool str_ieq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const char *yes_no(bool v) { return v ? "YES" : "NO"; }

static const char *net_state_name(NetworkMode m) {
    switch (m) {
        case NET_STATE_DISABLED:  return "DISABLED";
        case NET_STATE_DISCOVERY: return "DISCOVERY";
        case NET_STATE_BOUND:     return "BOUND";
        default:                  return "UNKNOWN";
    }
}

static const char *chip_name(void) {
#if defined(PICO_RP2350) && PICO_RP2350
    return "RP2350";
#else
    return "RP2040";
#endif
}

static const char *channel_name(uint8_t channel) {
    return channel == INMP441_CHANNEL_RIGHT ? "RIGHT" : "LEFT";
}

static const char *event_class_name(uint8_t cls) {
    switch (cls) {
        case ACOUSTIC_EVENT_IMPACT:    return "IMPACT";
        case ACOUSTIC_EVENT_CRACK:     return "CRACK";
        case ACOUSTIC_EVENT_TONAL:     return "TONAL";
        case ACOUSTIC_EVENT_TRANSIENT: return "TRANSIENT";
        default:                       return "NONE";
    }
}

static bool serial_auth_is_unlocked(void) {
    if (EDGE_SERIAL_ADMIN_TOKEN == 0ULL) return false;
    return g_serial_auth_until_ms != 0 && (int32_t)(g_serial_auth_until_ms - now_ms()) > 0;
}

// Mesma política do sensor de vibração: leitura livre, alteração autenticada.
static bool command_requires_auth(char *const tok[], int ntok) {
    if (ntok <= 0 || !tok[0]) return false;
    if (str_ieq(tok[0], "HELP") || str_ieq(tok[0], "MENU") || str_ieq(tok[0], "STATUS") ||
        str_ieq(tok[0], "GET") || str_ieq(tok[0], "PING") || str_ieq(tok[0], "VERSION") ||
        str_ieq(tok[0], "AUTH")) {
        return false;
    }
    if (str_ieq(tok[0], "DTC") && (ntok < 2 || !str_ieq(tok[1], "CLEAR"))) return false;
    if (str_ieq(tok[0], "NET")) {
        if (ntok < 2) return false;
        if (str_ieq(tok[1], "WIFI") && ntok >= 3 && str_ieq(tok[2], "STATUS")) return false;
        if (!str_ieq(tok[1], "WIFI")) return false;
    }
    return true;
}

static bool parse_float_strict(const char *tok, float *out) {
    if (!tok || !out) return false;
    char *end = NULL;
    const float v = strtof(tok, &end);
    if (end == tok || *end != '\0' || !isfinite(v)) return false;
    *out = v;
    return true;
}

static void print_prompt(void) {
    printf("EDGE> ");
    fflush(stdout);
}

static void print_ok(const char *msg) { printf("OK %s\n", msg); }
static void print_err(const char *msg) { printf("ERR %s\n", msg); }

static void seed_staged(void) {
    g_staged = *audio_app_current_config();
    g_staged.magic_header = NET_MAGIC_HEADER;
    g_staged.cmd_type = CMD_SET_CONFIG;
}

static void print_menu(void) {
    printf("\n");
    printf("EDGE DSP ACOUSTIC CONSOLE - ASCII MODE (INMP441)\n");
    printf("Commands:\n");
    printf("  HELP or MENU\n");
    printf("  STATUS | GET | VERSION | PING\n");
    printf("  SET MODE <IDLE|LEVEL|MACHINE|EVENTS|0-3>\n");
    printf("  SET RATE <8000|16000|24000|32000|48000>\n");
    printf("  SET FFT <256|512|1024|2048>          (alias WINDOW_SIZE)\n");
    printf("  SET WINDOW <RECT|HANN|HAMMING|FLATTOP|BLACKMAN|0-4>\n");
    printf("  SET THRESHOLD <value > 1>            (EVENTS: STA/LTA; MACHINE: z; alias STALTA)\n");
    printf("  SET GAIN <value>                     (pressure calibration gain)\n");
    printf("  APPLY\n");
    printf("  CAL <reference dB>                   (calibrator at 94 or 114 dB SPL, 1 kHz)\n");
    printf("  MEASURE RESET                        (Leq,T, Ln, doses and event count)\n");
    printf("  BASELINE LEARN <5..3600 s> | BASELINE CLEAR   (MACHINE mode)\n");
    printf("  TELEMETRY ON|OFF|ONCE|FAST|SLOW|PERIOD <ms>\n");
    printf("  FFT ONCE\n");
    printf("  DTC | DTC CLEAR\n");
    printf("  NET | NET WIFI ON|OFF|STATUS|PROVISION <ssid> <password>|CLEAR\n");
    printf("  AUTH STATUS|UNLOCK <token>|LOCK\n");
    printf("  RESET                                (then RESET CONFIRM within 10 s)\n");
    printf("Modes: 0=IDLE 1=LEVEL 2=MACHINE 3=EVENTS\n");
    printf("Wiring: SCK=GP%u WS=GP%u SD=GP%u, L/R to GND (left) or VDD (right, auto-detected)\n",
           (unsigned)AUDIO_PIN_SCK, (unsigned)AUDIO_PIN_WS, (unsigned)AUDIO_PIN_SD);
    printf("\n");
}

static void print_status(void) {
    const Payload_Configuration *cfg = audio_app_current_config();
    const BatteryStatus batt = battery_monitor_get_status();
    const bool idle = cfg->target_mode == ACOUSTIC_MODE_IDLE;
    const bool mic_ok = audio_core1_hardware_ok() && (!g_have_last || g_last.report.mic_state == ACOUSTIC_MIC_OK);
    printf("STATUS PROFILE=ACOUSTIC NET=%s MODE=%s ACQ=%s RATE_HZ=%.2f RATE_EFF_HZ=%.2f WINDOW=%s WINDOW_SIZE=%u "
           "THRESHOLD=%.3f STALTA=%.3f GAIN=%.4f CHANNEL=%s MIC=%s CPU_PCT=%u COVERAGE_PCT=%u CHIP=%s BOARD=%s "
           "DTC=0x%04X DTC_COUNT=%u TELEMETRY=%s PERIOD_MS=%u BATT_PCT=%u BATT_MV=%u AUTH=%s "
           "WIFI_PROVISIONED=%s BLE_BEACON=%s\n",
           net_state_name(edge_net_get_state()), audio_mode_name(cfg->target_mode), idle ? "IDLE" : "I2S",
           (double)cfg->sample_rate_hz, (double)audio_app_effective_rate_hz(), audio_window_name(cfg->window_type),
           (unsigned)cfg->window_size, (double)cfg->stalta_thresh, (double)cfg->stalta_thresh,
           (double)cfg->calib_gain, channel_name(audio_core1_channel()), yes_no(mic_ok),
           (unsigned)(g_have_last ? g_last.cpu_pct : 0u), (unsigned)(g_have_last ? g_last.report.coverage_pct : 0u),
           chip_name(), EDGE_BOARD_NAME, bite_get_current_status(), (unsigned)diagnostics_get_count(),
           g_telemetry_stream ? "ON" : "OFF", (unsigned)g_telemetry_period_ms, (unsigned)batt.pct,
           (unsigned)batt.mv, serial_auth_is_unlocked() ? "UNLOCKED" : "LOCKED",
           yes_no(edge_net_is_wifi_provisioned()), edge_ble_beacon_is_active() ? "ON" : "OFF");
}

static void print_staged(void) {
    printf("STAGED MODE=%s RATE_HZ=%.2f WINDOW=%s WINDOW_SIZE=%u THRESHOLD=%.3f GAIN=%.4f\n",
           audio_mode_name(g_staged.target_mode), (double)g_staged.sample_rate_hz,
           audio_window_name(g_staged.window_type), (unsigned)g_staged.window_size, (double)g_staged.stalta_thresh,
           (double)g_staged.calib_gain);
    printf("NOTE Use APPLY to commit staged values.\n");
}

static void queue_config(const Payload_Configuration *cfg) {
    g_pending_cfg = *cfg;
    g_pending_cfg_valid = true;
}

static void queue_control(uint8_t action, uint16_t argument) {
    g_pending_ctrl.valid = true;
    g_pending_ctrl.request_counter = 0;
    g_pending_ctrl.action = action;
    g_pending_ctrl.argument = argument;
}

static void command_set(char *tok[], int ntok) {
    if (ntok < 3) {
        print_err("Usage: SET <MODE|RATE|FFT|WINDOW|THRESHOLD|GAIN> <value>");
        return;
    }
    if (str_ieq(tok[1], "MODE")) {
        uint8_t mode = 0;
        if (!audio_parse_mode(tok[2], &mode)) {
            print_err("Invalid mode. Use IDLE, LEVEL, MACHINE, EVENTS or 0..3.");
            return;
        }
        g_staged.target_mode = mode;
        // Cada modo tem um limiar típico; o operador pode mudá-lo em seguida.
        if (mode == ACOUSTIC_MODE_MACHINE) g_staged.stalta_thresh = AUDIO_DEFAULT_THRESHOLD_MACHINE;
        if (mode == ACOUSTIC_MODE_EVENTS) g_staged.stalta_thresh = AUDIO_DEFAULT_THRESHOLD_EVENTS;
        printf("OK STAGED MODE=%s THRESHOLD=%.3f\n", audio_mode_name(mode), (double)g_staged.stalta_thresh);
        return;
    }
    if (str_ieq(tok[1], "RATE")) {
        float v = 0.0f;
        if (!parse_float_strict(tok[2], &v) || !edge_config_valid_sample_rate(v)) {
            print_err("Invalid rate. Use 8000, 16000, 24000, 32000 or 48000 Hz.");
            return;
        }
        g_staged.sample_rate_hz = v;
        printf("OK STAGED RATE_HZ=%.2f\n", (double)v);
        return;
    }
    if (str_ieq(tok[1], "FFT") || str_ieq(tok[1], "WINDOW_SIZE") || str_ieq(tok[1], "SIZE")) {
        char *end = NULL;
        const long v = strtol(tok[2], &end, 10);
        if (end == tok[2] || *end != '\0' || v < 0 || v > UINT16_MAX || !edge_config_valid_window_size((uint16_t)v)) {
            print_err("Invalid FFT size. Use 256, 512, 1024 or 2048.");
            return;
        }
        g_staged.window_size = (uint16_t)v;
        printf("OK STAGED WINDOW_SIZE=%u\n", (unsigned)g_staged.window_size);
        return;
    }
    if (str_ieq(tok[1], "WINDOW")) {
        uint8_t w = 0;
        if (!audio_parse_window(tok[2], &w)) {
            print_err("Invalid window. Use RECT, HANN, HAMMING, FLATTOP, BLACKMAN or 0..4.");
            return;
        }
        g_staged.window_type = w;
        printf("OK STAGED WINDOW=%s\n", audio_window_name(w));
        return;
    }
    if (str_ieq(tok[1], "THRESHOLD") || str_ieq(tok[1], "STALTA")) {
        float v = 0.0f;
        if (!parse_float_strict(tok[2], &v) || !edge_config_valid_stalta(v)) {
            print_err("Invalid threshold. Use value > 1.0.");
            return;
        }
        g_staged.stalta_thresh = v;
        printf("OK STAGED THRESHOLD=%.3f\n", (double)v);
        return;
    }
    if (str_ieq(tok[1], "GAIN")) {
        float v = 0.0f;
        if (!parse_float_strict(tok[2], &v) || !edge_config_valid_gain(v)) {
            print_err("Invalid gain. Use 0.01 < value <= 100.");
            return;
        }
        g_staged.calib_gain = v;
        printf("OK STAGED GAIN=%.4f\n", (double)v);
        return;
    }
    print_err("Unknown SET field.");
}

// Calibração com calibrador acústico (94 ou 114 dB SPL a 1 kHz, onde as
// ponderações A, C e Z coincidem): o ganho novo é o atual vezes a razão entre
// a pressão de referência e a medida, desde que as últimas leituras estejam
// estáveis (variação de até 0,5 dB).
static void command_cal(char *tok[], int ntok) {
    float reference = 0.0f;
    if (ntok < 2 || !parse_float_strict(tok[1], &reference) || reference < 70.0f || reference > 125.0f) {
        print_err("Usage: CAL <reference dB SPL, 70..125>");
        return;
    }
    const Payload_Configuration *cfg = audio_app_current_config();
    if (cfg->target_mode == ACOUSTIC_MODE_IDLE || g_cal_count < CAL_HISTORY) {
        print_err("CAL_NEEDS_3_READINGS Keep the calibrator on the microphone for 3 s in LEVEL/MACHINE/EVENTS mode.");
        return;
    }
    float lo = g_cal_lzeq[0], hi = g_cal_lzeq[0], sum = 0.0f;
    for (unsigned i = 0; i < CAL_HISTORY; ++i) {
        if (g_cal_lzeq[i] < lo) lo = g_cal_lzeq[i];
        if (g_cal_lzeq[i] > hi) hi = g_cal_lzeq[i];
        sum += g_cal_lzeq[i];
    }
    if (hi - lo > 0.5f) {
        printf("ERR CAL_UNSTABLE SPREAD_DB=%.2f\n", (double)(hi - lo));
        return;
    }
    const float measured = sum / (float)CAL_HISTORY;
    const float offset = reference - measured;
    const float gain = cfg->calib_gain * powf(10.0f, offset / 20.0f);
    if (!edge_config_valid_gain(gain)) {
        print_err("CAL_OUT_OF_RANGE Check the calibrator and the wiring.");
        return;
    }
    Payload_Configuration next = *cfg;
    next.calib_gain = gain;
    next.magic_header = NET_MAGIC_HEADER;
    next.cmd_type = CMD_SET_CONFIG;
    queue_config(&next);
    g_staged = next;
    g_cal_count = 0;
    printf("OK CAL MEASURED_DB=%.2f REFERENCE_DB=%.2f OFFSET_DB=%+.2f GAIN=%.4f\n", (double)measured,
           (double)reference, (double)offset, (double)gain);
}

static void process_line(char *line) {
    char *tok[6] = {0};
    int ntok = 0;
    for (char *p = strtok(line, " \t"); p && ntok < 6; p = strtok(NULL, " \t")) tok[ntok++] = p;
    if (ntok == 0) return;

    if (str_ieq(tok[0], "AUTH")) {
        if (ntok >= 2 && str_ieq(tok[1], "STATUS")) {
            printf("AUTH STATE=%s PROVISIONED=%s LEASE_MS=%u\n", serial_auth_is_unlocked() ? "UNLOCKED" : "LOCKED",
                   EDGE_SERIAL_ADMIN_TOKEN != 0ULL ? "YES" : "NO",
                   serial_auth_is_unlocked() ? (unsigned)(g_serial_auth_until_ms - now_ms()) : 0u);
            return;
        }
        if (ntok >= 2 && str_ieq(tok[1], "LOCK")) {
            g_serial_auth_until_ms = 0;
            g_reset_confirm_until_ms = 0;
            print_ok("AUTH_LOCKED");
            return;
        }
        if (ntok >= 3 && str_ieq(tok[1], "UNLOCK")) {
            if (EDGE_SERIAL_ADMIN_TOKEN == 0ULL) {
                print_err("AUTH_NOT_PROVISIONED");
                return;
            }
            char *end = NULL;
            errno = 0;
            const unsigned long long candidate = strtoull(tok[2], &end, 0);
            if (errno == ERANGE || end == tok[2] || *end != '\0' ||
                (uint64_t)candidate != (uint64_t)EDGE_SERIAL_ADMIN_TOKEN) {
                print_err("AUTH_REJECTED");
                return;
            }
            g_serial_auth_until_ms = now_ms() + SERIAL_AUTH_LEASE_MS;
            print_ok("AUTH_UNLOCKED");
            return;
        }
        print_err("Usage: AUTH STATUS|UNLOCK <token>|LOCK");
        return;
    }

    if (command_requires_auth(tok, ntok) && !serial_auth_is_unlocked()) {
        print_err(EDGE_SERIAL_ADMIN_TOKEN == 0ULL ? "AUTH_NOT_PROVISIONED" : "AUTH_REQUIRED");
        return;
    }

    if (str_ieq(tok[0], "HELP") || str_ieq(tok[0], "MENU")) {
        print_menu();
    } else if (str_ieq(tok[0], "STATUS")) {
        print_status();
    } else if (str_ieq(tok[0], "GET")) {
        print_staged();
    } else if (str_ieq(tok[0], "SET")) {
        command_set(tok, ntok);
    } else if (str_ieq(tok[0], "APPLY")) {
        EdgeConfigValidationError err;
        if (!edge_config_validate(&g_staged, &err)) {
            printf("ERR CONFIG_INVALID FIELD=%s\n", edge_config_validation_error_name(err));
            return;
        }
        queue_config(&g_staged);
        print_ok("APPLY_QUEUED");
    } else if (str_ieq(tok[0], "CAL")) {
        command_cal(tok, ntok);
    } else if (str_ieq(tok[0], "MEASURE")) {
        if (ntok >= 2 && str_ieq(tok[1], "RESET")) {
            queue_control(ACOUSTIC_CTRL_MEASURE_RESET, 0);
            print_ok("MEASURE_RESET_QUEUED");
        } else {
            print_err("Usage: MEASURE RESET");
        }
    } else if (str_ieq(tok[0], "BASELINE")) {
        if (ntok >= 3 && str_ieq(tok[1], "LEARN")) {
            char *end = NULL;
            const long seconds = strtol(tok[2], &end, 10);
            if (end == tok[2] || *end != '\0' || seconds < 5 || seconds > 3600) {
                print_err("Usage: BASELINE LEARN <5..3600>");
                return;
            }
            queue_control(ACOUSTIC_CTRL_BASELINE_LEARN, (uint16_t)seconds);
            print_ok("BASELINE_LEARN_QUEUED");
        } else if (ntok >= 2 && str_ieq(tok[1], "CLEAR")) {
            queue_control(ACOUSTIC_CTRL_BASELINE_CLEAR, 0);
            print_ok("BASELINE_CLEAR_QUEUED");
        } else {
            print_err("Usage: BASELINE LEARN <seconds> | BASELINE CLEAR");
        }
    } else if (str_ieq(tok[0], "TELEMETRY") || str_ieq(tok[0], "TEL")) {
        if (ntok < 2) {
            print_err("Usage: TELEMETRY ON|OFF|ONCE|FAST|SLOW|PERIOD <ms>");
        } else if (str_ieq(tok[1], "ON")) {
            g_telemetry_stream = true;
            g_last_telemetry_ms = 0;
            printf("OK TELEMETRY=ON PERIOD_MS=%u\n", (unsigned)g_telemetry_period_ms);
        } else if (str_ieq(tok[1], "OFF")) {
            g_telemetry_stream = false;
            g_telemetry_once = false;
            print_ok("TELEMETRY=OFF");
        } else if (str_ieq(tok[1], "ONCE")) {
            g_telemetry_once = true;
            print_ok("TELEMETRY=ONCE");
        } else if (str_ieq(tok[1], "FAST")) {
            g_telemetry_period_ms = 1000;   // um relatório por segundo é o máximo deste sensor
            printf("OK TELEMETRY_PERIOD_MS=%u\n", (unsigned)g_telemetry_period_ms);
        } else if (str_ieq(tok[1], "SLOW")) {
            g_telemetry_period_ms = 5000;
            printf("OK TELEMETRY_PERIOD_MS=%u\n", (unsigned)g_telemetry_period_ms);
        } else if (str_ieq(tok[1], "PERIOD") && ntok >= 3) {
            char *end = NULL;
            const long v = strtol(tok[2], &end, 10);
            if (end == tok[2] || *end != '\0' || v < 100 || v > 60000) {
                print_err("Invalid telemetry period. Use 100..60000 ms.");
                return;
            }
            g_telemetry_period_ms = (uint32_t)v;
            printf("OK TELEMETRY_PERIOD_MS=%u\n", (unsigned)g_telemetry_period_ms);
        } else {
            print_err("Usage: TELEMETRY ON|OFF|ONCE|FAST|SLOW|PERIOD <ms>");
        }
    } else if (str_ieq(tok[0], "FFT")) {
        if (ntok >= 2 && str_ieq(tok[1], "ONCE")) {
            g_fft_once = true;
            print_ok("FFT_ONCE");
        } else {
            print_err("Usage: FFT ONCE");
        }
    } else if (str_ieq(tok[0], "PING")) {
        printf("PONG UPTIME_MS=%u\n", (unsigned)now_ms());
    } else if (str_ieq(tok[0], "DTC")) {
        if (ntok >= 2 && str_ieq(tok[1], "CLEAR")) {
            diagnostics_clear_all();
            print_ok("DTC_CLEAR");
        } else {
            printf("DTC ACTIVE=0x%04X COUNT=%u\n", bite_get_current_status(), (unsigned)diagnostics_get_count());
        }
    } else if (str_ieq(tok[0], "NET")) {
        if (ntok >= 2 && str_ieq(tok[1], "WIFI")) {
            if (ntok >= 3 && str_ieq(tok[2], "ON")) {
                const bool ok = edge_net_set_enabled(true);
                printf("%s NET_WIFI=ON STATE=%s\n", ok ? "OK" : "ERR", net_state_name(edge_net_get_state()));
            } else if (ntok >= 3 && str_ieq(tok[2], "OFF")) {
                const bool ok = edge_net_set_enabled(false);
                printf("%s NET_WIFI=OFF STATE=%s\n", ok ? "OK" : "ERR", net_state_name(edge_net_get_state()));
            } else if (ntok >= 3 && str_ieq(tok[2], "STATUS")) {
                printf("NET WIFI=%s STATE=%s PROVISIONED=%s BLE_BEACON=%s\n", edge_net_is_enabled() ? "ON" : "OFF",
                       net_state_name(edge_net_get_state()), yes_no(edge_net_is_wifi_provisioned()),
                       edge_ble_beacon_is_active() ? "ON" : "OFF");
            } else if (ntok >= 5 && str_ieq(tok[2], "PROVISION")) {
                const bool ok = edge_net_provision_wifi(tok[3], tok[4]);
                printf("%s NET_WIFI_PROVISIONED=%s\n", ok ? "OK" : "ERR", ok ? "YES" : "NO");
            } else if (ntok >= 3 && str_ieq(tok[2], "CLEAR")) {
                edge_net_clear_wifi_provisioning();
                print_ok("NET_WIFI_PROVISIONING_CLEARED");
            } else {
                print_err("Usage: NET WIFI ON|OFF|STATUS|PROVISION <ssid> <password>|CLEAR");
            }
            return;
        }
        printf("NET STATE=%s WIFI=%s PROVISIONED=%s BLE_BEACON=%s LINK_KEYS=%s PARENT_NODE=%u CHILD=%u "
               "AUTH_REJECT=%u REPLAY_REJECT=%u\n",
               net_state_name(edge_net_get_state()), edge_net_is_enabled() ? "ON" : "OFF",
               yes_no(edge_net_is_wifi_provisioned()), edge_ble_beacon_is_active() ? "ON" : "OFF",
               edge_link_key_mode_name(), (unsigned)edge_net_parent_node_id(), (unsigned)edge_net_child_id(),
               (unsigned)edge_net_auth_reject_count(), (unsigned)edge_net_replay_reject_count());
    } else if (str_ieq(tok[0], "VERSION")) {
        printf("VERSION PROTOCOL=%u NODE_UUID=0x%016llX PROFILE=ACOUSTIC CHIP=%s BOARD=%s\n",
               (unsigned)NET_PROTOCOL_VERSION, (unsigned long long)edge_device_uuid64(), chip_name(),
               EDGE_BOARD_NAME);
    } else if (str_ieq(tok[0], "RESET")) {
        const uint32_t now = now_ms();
        if (ntok >= 2 && str_ieq(tok[1], "CONFIRM")) {
            if (g_reset_confirm_until_ms == 0 || (int32_t)(g_reset_confirm_until_ms - now) <= 0) {
                g_reset_confirm_until_ms = 0;
                print_err("RESET_CONFIRMATION_EXPIRED");
                return;
            }
            g_reset_confirm_until_ms = 0;
            print_ok("RESET_CONFIRMED");
            sleep_ms(100);
            watchdog_reboot(0, 0, 0);
            return;
        }
        g_reset_confirm_until_ms = now + RESET_CONFIRM_WINDOW_MS;
        print_ok("RESET_PENDING CONFIRM_WITH='RESET CONFIRM' WINDOW_MS=10000");
    } else {
        print_err("Unknown command. Use HELP.");
    }
}

static void print_welcome(void) {
    printf("\nEDGE DSP ACOUSTIC NODE (INMP441, %s)\n", chip_name());
    printf("ASCII serial console ready. Type HELP and press Enter.\n");
    print_menu();
    print_prompt();
}

static void maybe_print_welcome(void) {
    static bool was_connected = false;
    static bool boot_welcome_done = false;
    static uint32_t boot_ms = 0;
    if (boot_ms == 0) boot_ms = now_ms();
    const bool connected = stdio_usb_connected();
    if (connected && !was_connected) {
        seed_staged();
        print_welcome();
        boot_welcome_done = true;
    }
    was_connected = connected;
    if (!boot_welcome_done && now_ms() - boot_ms > BOOT_WELCOME_TIMEOUT_MS) {
        print_welcome();
        boot_welcome_done = true;
    }
}

static void handle_char(int c) {
    if (c == PICO_ERROR_TIMEOUT) return;
    if (c == 0x03 || c == '!') {   // Ctrl+C ou '!': interrompe a telemetria sem Enter
        g_telemetry_stream = false;
        g_telemetry_once = false;
        g_linelen = 0;
        printf("\r\nOK TELEMETRY=OFF\n");
        print_prompt();
        return;
    }
    if (c == '\r' || c == '\n') {
        printf("\r\n");
        if (g_linelen > 0) {
            g_linebuf[g_linelen] = '\0';
            process_line(g_linebuf);
            g_linelen = 0;
        }
        print_prompt();
        return;
    }
    if (c == 0x08 || c == 0x7F) {
        if (g_linelen > 0) {
            g_linelen--;
            printf("\b \b");
            fflush(stdout);
        }
        return;
    }
    if (c < 0x20 || c > 0x7E) return;
    if (g_linelen < LINE_BUF_SIZE - 1u) {
        g_linebuf[g_linelen++] = (char)c;
        putchar(c);
        fflush(stdout);
    } else {
        print_err("Line too long. Buffer cleared.");
        g_linelen = 0;
        print_prompt();
    }
}

void audio_console_init(void) {
    g_linelen = 0;
    g_telemetry_stream = false;
    g_telemetry_once = false;
    g_fft_once = false;
    g_telemetry_period_ms = 1000;
    g_last_telemetry_ms = 0;
    g_serial_auth_until_ms = 0;
    g_reset_confirm_until_ms = 0;
    g_pending_cfg_valid = false;
    memset(&g_pending_ctrl, 0, sizeof(g_pending_ctrl));
    g_have_last = false;
    g_cal_count = 0;
    seed_staged();
}

void audio_console_poll(void) {
    maybe_print_welcome();
    for (int guard = 0; guard < 64; ++guard) {
        const int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) break;
        handle_char(c);
    }
}

bool audio_console_take_pending_config(Payload_Configuration *out) {
    if (!g_pending_cfg_valid) return false;
    if (out) *out = g_pending_cfg;
    g_pending_cfg_valid = false;
    return true;
}

bool audio_console_take_pending_control(AudioPendingControl *out) {
    if (!g_pending_ctrl.valid) return false;
    if (out) *out = g_pending_ctrl;
    g_pending_ctrl.valid = false;
    return true;
}

static void print_db(const char *key, float db) {
    if (db == db) printf(" %s=%.2f", key, (double)db);
}

static void print_tel(const AudioReportMessage *msg) {
    const AcousticReport *r = &msg->report;
    printf("\r\nTEL PROFILE=ACOUSTIC MODE=%s ACQ=I2S WIN=%u RATE_EFF_HZ=%.2f INTERVAL_MS=%u COVERAGE_PCT=%u "
           "CPU_PCT=%u CHANNEL=%s SPECTRUM_VALID=%s OVERLOAD=%s MIC_FAULT=%s",
           audio_mode_name(r->mode), (unsigned)r->fft_size, (double)r->sample_rate_hz,
           (unsigned)lroundf(1000.0f * (float)r->samples / r->sample_rate_hz), (unsigned)r->coverage_pct,
           (unsigned)msg->cpu_pct, channel_name(msg->channel), yes_no(r->flags & ACOUSTIC_FLAG_SPECTRUM_VALID),
           yes_no(r->flags & ACOUSTIC_FLAG_OVERLOAD), yes_no(r->flags & ACOUSTIC_FLAG_MIC_FAULT));
    print_db("LAEQ_DB", r->laeq);
    print_db("LCEQ_DB", r->lceq);
    print_db("LZEQ_DB", r->lzeq);
    print_db("LZPEAK_DB", r->lzpeak);
    print_db("LAF_DB", r->laf);
    print_db("LAFMAX_DB", r->lafmax);
    print_db("LAFMIN_DB", r->lafmin);
    if (r->crest == r->crest) printf(" CREST=%.2f", (double)r->crest);
    if (r->kurtosis == r->kurtosis) printf(" KURT=%.2f", (double)r->kurtosis);
    if (r->peak_hz == r->peak_hz) printf(" PEAK_HZ=%.1f", (double)r->peak_hz);
    if (r->centroid_hz == r->centroid_hz) printf(" CENTROID_HZ=%u", (unsigned)lroundf(r->centroid_hz));
    if (r->flatness == r->flatness) printf(" FLATNESS=%.4f", (double)r->flatness);
    printf(" OCT_DB=");
    for (unsigned b = 0; b < ACOUSTIC_BANDS; ++b) {
        if (r->octave_db[b] == r->octave_db[b]) printf("%s%.1f", b ? "," : "", (double)r->octave_db[b]);
        else printf("%s-", b ? "," : "");
    }
    switch (r->mode) {
        case ACOUSTIC_MODE_LEVEL:
            print_db("LEQ_T_DB", r->leq_total);
            print_db("L10_DB", r->l10);
            print_db("L50_DB", r->l50);
            print_db("L90_DB", r->l90);
            printf(" DOSE_NR15_PCT=%.1f DOSE_NHO01_PCT=%.1f ELAPSED_S=%u DOSE_LIMIT=%s", (double)r->dose_nr15_pct,
                   (double)r->dose_nho01_pct, (unsigned)r->elapsed_s, yes_no(r->flags & ACOUSTIC_FLAG_DOSE_LIMIT));
            break;
        case ACOUSTIC_MODE_MACHINE: {
            const char *baseline = (r->flags & ACOUSTIC_FLAG_BASELINE_READY)      ? "READY"
                                   : (r->flags & ACOUSTIC_FLAG_BASELINE_LEARNING) ? "LEARNING"
                                                                                   : "NONE";
            if (r->tonal_db == r->tonal_db) printf(" TONAL_DB=%.1f", (double)r->tonal_db);
            printf(" F0_HZ=%.2f HARMONICS=%u", (double)(r->f0_hz == r->f0_hz ? r->f0_hz : 0.0f),
                   (unsigned)r->harmonics);
            if (r->anomaly_z == r->anomaly_z) printf(" ANOMALY_Z=%.2f", (double)r->anomaly_z);
            printf(" BASELINE=%s BASELINE_PCT=%u ANOMALY=%s", baseline, (unsigned)r->baseline_pct,
                   yes_no(r->flags & ACOUSTIC_FLAG_ANOMALY));
            break;
        }
        case ACOUSTIC_MODE_EVENTS:
            printf(" EVENTS=%u EVENTS_INTERVAL=%u EVENT_ACTIVE=%s", (unsigned)r->event_count,
                   (unsigned)r->events_interval, yes_no(r->flags & ACOUSTIC_FLAG_EVENT_ACTIVE));
            if (r->sta_lta_db == r->sta_lta_db) printf(" STA_LTA_DB=%.1f", (double)r->sta_lta_db);
            if (r->background_db == r->background_db) printf(" BACKGROUND_DB=%.1f", (double)r->background_db);
            if (r->event_age_s != ACOUSTIC_AGE_NONE) {
                printf(" EVENT_CLASS=%s", event_class_name(r->event_class));
                print_db("EVENT_PEAK_DB", r->event_peak_db);
                print_db("EVENT_SEL_DB", r->event_sel_db);
                printf(" EVENT_DURATION_MS=%u", (unsigned)r->event_duration_ms);
                if (r->event_centroid_hz == r->event_centroid_hz) {
                    printf(" EVENT_CENTROID_HZ=%u", (unsigned)lroundf(r->event_centroid_hz));
                }
                printf(" EVENT_AGE_S=%u", (unsigned)r->event_age_s);
            }
            break;
        default:
            break;
    }
    const BatteryStatus batt = battery_monitor_get_status();
    printf(" BATT_PCT=%u BATT_MV=%u DTC=0x%04X DTC_COUNT=%u\n", (unsigned)batt.pct, (unsigned)batt.mv,
           bite_get_current_status(), (unsigned)diagnostics_get_count());
}

static void print_fft(const AcousticReport *r) {
    if (!r->spectrum_bins) {
        printf("\r\nFFT VALID=NO BINS=0\n");
        return;
    }
    // As faixas cobrem 0..fs/2: o tamanho equivalente da FFT é 2 · faixas.
    printf("\r\nFFT VALID=YES BINS=%u RATE_HZ=%.2f FFT_SIZE=%u UNIT=PA VALUES=", (unsigned)r->spectrum_bins,
           (double)r->sample_rate_hz, (unsigned)(2u * r->spectrum_bins));
    for (uint16_t i = 0; i < r->spectrum_bins; ++i) {
        printf("%s%.6g", i ? "," : "", (double)r->spectrum_pa[i]);
        if ((i & 31u) == 31u) watchdog_update();
    }
    printf("\n");
}

void audio_console_on_report(const AudioReportMessage *msg) {
    if (!msg) return;
    g_last = *msg;
    g_have_last = true;
    const AcousticReport *r = &msg->report;
    if (r->lzeq == r->lzeq) {
        // Histórico para CAL: só o último trio de leituras conta.
        if (g_cal_count < CAL_HISTORY) {
            g_cal_lzeq[g_cal_count++] = r->lzeq;
        } else {
            memmove(&g_cal_lzeq[0], &g_cal_lzeq[1], sizeof(float) * (CAL_HISTORY - 1));
            g_cal_lzeq[CAL_HISTORY - 1] = r->lzeq;
        }
    }
    if (msg->channel_switched) {
        printf("\r\n[MIC] sem sinal no canal anterior; tentando o canal %s (pino L/R em %s)\n",
               msg->channel == INMP441_CHANNEL_RIGHT ? "direito" : "esquerdo",
               msg->channel == INMP441_CHANNEL_RIGHT ? "VDD" : "GND");
        print_prompt();
    }
    if (r->mic_state != g_last_mic_state) {
        if (r->mic_state == ACOUSTIC_MIC_OK) {
            printf("\r\n[MIC] sinal presente no canal %s\n", msg->channel == INMP441_CHANNEL_RIGHT ? "direito" : "esquerdo");
        } else {
            printf("\r\n[MIC] %s: confira alimentação, SCK=GP%u, WS=GP%u, SD=GP%u e o pino L/R\n",
                   r->mic_state == ACOUSTIC_MIC_SILENT ? "linha SD sempre em 0" : "linha SD presa em 1",
                   (unsigned)AUDIO_PIN_SCK, (unsigned)AUDIO_PIN_WS, (unsigned)AUDIO_PIN_SD);
        }
        g_last_mic_state = r->mic_state;
        print_prompt();
    }

    const uint32_t now = now_ms();
    const bool due = g_telemetry_stream &&
                     (g_last_telemetry_ms == 0 || now - g_last_telemetry_ms + 50u >= g_telemetry_period_ms);
    if (g_telemetry_once || due) {
        print_tel(msg);
        g_last_telemetry_ms = now;
        if (g_telemetry_once && !g_telemetry_stream) {
            g_telemetry_once = false;
            print_prompt();
        }
        g_telemetry_once = false;
    }
    if (g_fft_once) {
        g_fft_once = false;
        print_fft(r);
        print_prompt();
    }
}

void audio_console_report_config_result(const AudioCommandResult *res) {
    if (!res) return;
    const Payload_Configuration *cfg = &res->config;
    if (res->ok) {
        seed_staged();
        printf("\r\nCONFIG_APPLIED MODE=%s RATE_REQ=%.2f RATE_EFF=%.2f WINDOW=%s WINDOW_SIZE=%u THRESHOLD=%.3f "
               "STALTA=%.3f GAIN=%.4f ACQ=%s\n",
               audio_mode_name(cfg->target_mode), (double)cfg->sample_rate_hz, (double)res->effective_rate_hz,
               audio_window_name(cfg->window_type), (unsigned)cfg->window_size, (double)cfg->stalta_thresh,
               (double)cfg->stalta_thresh, (double)cfg->calib_gain,
               cfg->target_mode == ACOUSTIC_MODE_IDLE ? "IDLE" : "I2S");
    } else {
        printf("\r\nERR CONFIG_REJECTED MODE=%s RATE_REQ=%.2f\n", audio_mode_name(cfg->target_mode),
               (double)cfg->sample_rate_hz);
    }
    print_prompt();
}

void audio_console_report_control_result(const AudioCommandResult *res) {
    if (!res) return;
    static const char *const names[] = {"UNKNOWN", "MEASURE_RESET", "BASELINE_LEARN", "BASELINE_CLEAR"};
    const char *name = res->action <= ACOUSTIC_CTRL_BASELINE_CLEAR ? names[res->action] : names[0];
    if (res->ok) printf("\r\nOK %s\n", name);
    else printf("\r\nERR %s_REJECTED%s\n", name,
                res->action == ACOUSTIC_CTRL_BASELINE_LEARN ? " (use MODE=MACHINE and 5..3600 s)" : "");
    print_prompt();
}

void audio_console_report_dtc(const DTC_Record *rec) {
    if (!rec) return;
    printf("\r\nDTC_EVENT CODE=0x%04X SYMPTOM=0x%02X SEVERITY=%u TS_MS=%u FSM=%u MPU_TEMP_C=%d MCU_TEMP_C=%d "
           "VCC_MV=%u\n",
           rec->dtc_code, rec->symptom, rec->severity, (unsigned)rec->freeze.timestamp_ms,
           (unsigned)rec->freeze.fsm_state, (int)rec->freeze.mpu_temp_c, (int)rec->freeze.mcu_temp_c,
           (unsigned)rec->freeze.vcc_mv);
    print_prompt();
}

void audio_console_report_net_state(NetworkMode mode) {
    printf("\r\nNET_EVENT STATE=%s\n", net_state_name(mode));
    print_prompt();
}
