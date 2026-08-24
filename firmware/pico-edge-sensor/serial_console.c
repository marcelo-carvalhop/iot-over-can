/**
 * @file serial_console.c
 * @brief Console ASCII simples via USB CDC.
 *
 * Protocolo:
 * - Um comando ASCII por linha.
 * - A linha pode terminar em '\n', '\r' ou '\r\n'.
 * - As respostas são texto simples, sem JSON.
 * - O firmware ecoa os caracteres recebidos para facilitar uso com picocom.
 */

#include "serial_console.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/watchdog.h"
#include "mpu6050_dma_driver.h"
#include "battery_monitor.h"

extern OperationModeFSM main_get_active_fsm_mode(void);
extern uint16_t         bite_get_current_status(void);
extern bool             main_is_mpu_present(void);
extern AcquisitionMode  main_get_acquisition_mode(void);
extern bool             main_restart_polling_acquisition(void);
extern uint8_t          diagnostics_get_count(void);
extern void             diagnostics_clear_all(void);

#define LINE_BUF_SIZE 160

static char     g_linebuf[LINE_BUF_SIZE];
static uint16_t g_linelen = 0;

static bool g_telemetry_stream = false;
static bool g_telemetry_once   = false;
static bool g_simulate_mode    = false;
static bool g_prompt_printed   = false;
static bool g_fft_once         = false;

// Controle de vazão da telemetria serial. TELEMETRY ON não deve inundar o
// terminal, porque o usuário ainda precisa conseguir digitar TELEMETRY OFF.
static uint32_t g_telemetry_period_ms = 1000;
static uint32_t g_last_telemetry_tx_ms = 0;

static Payload_Configuration g_staged;

#define BOOT_WELCOME_TIMEOUT_MS 1500

static const char *yes_no(bool v) {
    return v ? "YES" : "NO";
}

static const char *net_state_name(NetworkMode m) {
    switch (m) {
        case NET_STATE_DISABLED:  return "DISABLED";
        case NET_STATE_DISCOVERY: return "DISCOVERY";
        case NET_STATE_BOUND:     return "BOUND";
        default:                  return "UNKNOWN";
    }
}

static const char *fsm_mode_name(uint8_t m) {
    switch (m) {
        case FSM_MODE_IDLE:           return "IDLE";
        case FSM_MODE_ROTATING_MACH:  return "ROTATING";
        case FSM_MODE_STRUCTURAL:     return "STRUCTURAL";
        case FSM_MODE_SEISMIC_STALTA: return "SEISMIC";
        default:                      return "UNKNOWN";
    }
}

static const char *acq_mode_name(uint8_t m) {
    switch (m) {
        case ACQ_MODE_DRDY:      return "DRDY";
        case ACQ_MODE_POLLING:   return "POLLING";
        case ACQ_MODE_SIMULATED: return "SIM";
        case ACQ_MODE_IDLE:      return "IDLE";
        default:                 return "UNKNOWN";
    }
}

static const char *window_name(uint8_t w) {
    switch (w) {
        case WIN_RECTANGULAR: return "RECT";
        case WIN_HANN:        return "HANN";
        case WIN_HAMMING:     return "HAMMING";
        case WIN_FLATTOP:     return "FLATTOP";
        case WIN_BLACKMAN_H:  return "BLACKMAN";
        default:              return "UNKNOWN";
    }
}

static bool str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool parse_fsm_mode(const char *tok, uint8_t *out) {
    if (!tok || !out) return false;

    if (str_ieq(tok, "IDLE"))       { *out = FSM_MODE_IDLE; return true; }
    if (str_ieq(tok, "ROTATING"))   { *out = FSM_MODE_ROTATING_MACH; return true; }
    if (str_ieq(tok, "ROT"))        { *out = FSM_MODE_ROTATING_MACH; return true; }
    if (str_ieq(tok, "STRUCTURAL")) { *out = FSM_MODE_STRUCTURAL; return true; }
    if (str_ieq(tok, "STRUCT"))     { *out = FSM_MODE_STRUCTURAL; return true; }
    if (str_ieq(tok, "SEISMIC"))    { *out = FSM_MODE_SEISMIC_STALTA; return true; }

    char *endp = NULL;
    long v = strtol(tok, &endp, 10);
    if (endp != tok && *endp == '\0' && v >= FSM_MODE_IDLE && v <= FSM_MODE_SEISMIC_STALTA) {
        *out = (uint8_t)v;
        return true;
    }

    return false;
}

static bool parse_window(const char *tok, uint8_t *out) {
    if (!tok || !out) return false;

    if (str_ieq(tok, "RECT"))      { *out = WIN_RECTANGULAR; return true; }
    if (str_ieq(tok, "RECTANGULAR")) { *out = WIN_RECTANGULAR; return true; }
    if (str_ieq(tok, "HANN"))      { *out = WIN_HANN; return true; }
    if (str_ieq(tok, "HAMMING"))   { *out = WIN_HAMMING; return true; }
    if (str_ieq(tok, "FLATTOP"))   { *out = WIN_FLATTOP; return true; }
    if (str_ieq(tok, "BLACKMAN"))  { *out = WIN_BLACKMAN_H; return true; }

    char *endp = NULL;
    long v = strtol(tok, &endp, 10);
    if (endp != tok && *endp == '\0' && v >= WIN_RECTANGULAR && v <= WIN_BLACKMAN_H) {
        *out = (uint8_t)v;
        return true;
    }

    return false;
}

static bool parse_float_strict(const char *tok, float *out) {
    if (!tok || !out) return false;
    char *endp = NULL;
    float v = strtof(tok, &endp);
    if (endp == tok || *endp != '\0') return false;
    *out = v;
    return true;
}

static void serial_print_prompt(void) {
    printf("EDGE> ");
    fflush(stdout);
    g_prompt_printed = true;
}

static void seed_staged_from_live(void) {
    g_staged.magic_header    = NET_MAGIC_HEADER;
    g_staged.cmd_type        = CMD_SET_CONFIG;
    g_staged.session_token   = 0;
    g_staged.target_mode     = (uint8_t)main_get_active_fsm_mode();
    g_staged.sample_rate_hz  = mpu6050_get_configured_sample_rate_hz();
    g_staged.window_size     = dsp_get_active_window_size();
    g_staged.window_type     = (uint8_t)dsp_get_current_window();
    g_staged.stalta_thresh   = dsp_get_stalta_threshold();
    g_staged.calib_gain      = dsp_get_calibration_gain();
}

static void print_menu(void) {
    printf("\n");
    printf("EDGE DSP SERIAL CONSOLE - ASCII MODE\n");
    printf("Commands:\n");
    printf("  HELP or MENU\n");
    printf("  STATUS\n");
    printf("  GET\n");
    printf("  SET MODE <IDLE|ROTATING|STRUCTURAL|SEISMIC|0-3>\n");
    printf("  SET RATE <4..1000>\n");
    printf("  SET WINDOW <RECT|HANN|HAMMING|FLATTOP|BLACKMAN|0-4>\n");
    printf("  SET WINDOW_SIZE <128|256|512>\n");
    printf("  SET STALTA <value > 1.0>\n");
    printf("  SET GAIN <value > 0.0>\n");
    printf("  APPLY\n");
    printf("  TELEMETRY ON|OFF|ONCE\n");
    printf("  FFT ONCE                         (prints vector after next FFT-valid buffer)\n");
    printf("  TELEMETRY FAST|SLOW|PERIOD <ms>   (default: 1000 ms)\n");
    printf("  Shortcuts while typing: ! or Ctrl+C stops telemetry\n");
    printf("  SIMULATE ON|OFF\n");
    printf("  PING\n");
    printf("  DTC\n");
    printf("  ACQ POLLING              (restart polling acquisition)\n");
    printf("  DTC CLEAR\n");
    printf("  NET\n");
    printf("  NET WIFI ON|OFF|STATUS       (wireless disabled at boot)\n");
    printf("  VERSION\n");
    printf("  RESET\n");
    printf("\n");
    printf("Ranges: RATE=4..1000 Hz, WINDOW_SIZE=128|256|512 samples\n");
    printf("Modes: 0=IDLE 1=ROTATING 2=STRUCTURAL 3=SEISMIC\n");
    printf("Windows: 0=RECT 1=HANN 2=HAMMING 3=FLATTOP 4=BLACKMAN\n");
    printf("\n");
}

static void print_status(void) {
    BatteryStatus batt = battery_monitor_get_status();
    printf("STATUS NET=%s MODE=%s ACQ=%s WINDOW=%s WINDOW_SIZE=%u RATE_HZ=%.2f STALTA=%.3f GAIN=%.3f "
           "DTC=0x%04X DTC_COUNT=%u MPU=%s SIM=%s TELEMETRY=%s PERIOD_MS=%u "
           "BATT_PCT=%u BATT_MV=%u DRDY_IRQ=%u DRDY_MISSED=%u\n",
           net_state_name(edge_net_get_state()),
           fsm_mode_name((uint8_t)main_get_active_fsm_mode()),
           acq_mode_name((uint8_t)main_get_acquisition_mode()),
           window_name((uint8_t)dsp_get_current_window()),
           (unsigned)dsp_get_active_window_size(),
           (double)mpu6050_get_configured_sample_rate_hz(),
           (double)dsp_get_stalta_threshold(),
           (double)dsp_get_calibration_gain(),
           bite_get_current_status(),
           (unsigned)diagnostics_get_count(),
           yes_no(main_is_mpu_present()),
           yes_no(g_simulate_mode),
           g_telemetry_stream ? "ON" : "OFF",
           (unsigned)g_telemetry_period_ms,
           (unsigned)batt.pct,
           (unsigned)batt.mv,
           (unsigned)mpu6050_get_drdy_irq_count(),
           (unsigned)mpu6050_get_drdy_missed_count());
}

static void print_staged(void) {
    printf("STAGED MODE=%s RATE_HZ=%.2f WINDOW=%s WINDOW_SIZE=%u STALTA=%.3f GAIN=%.3f\n",
           fsm_mode_name(g_staged.target_mode),
           (double)g_staged.sample_rate_hz,
           window_name(g_staged.window_type),
           (unsigned)g_staged.window_size,
           (double)g_staged.stalta_thresh,
           (double)g_staged.calib_gain);
    printf("NOTE Use APPLY to commit staged values.\n");
}

static void print_ok(const char *msg) {
    printf("OK %s\n", msg);
}

static void print_err(const char *msg) {
    printf("ERR %s\n", msg);
}

static void process_line(char *line) {
    char *tok[4] = {0};
    int ntok = 0;

    char *p = strtok(line, " \t");
    while (p && ntok < 4) {
        tok[ntok++] = p;
        p = strtok(NULL, " \t");
    }

    if (ntok == 0) {
        return;
    }

    if (str_ieq(tok[0], "HELP") || str_ieq(tok[0], "MENU")) {
        print_menu();
        return;
    }

    if (str_ieq(tok[0], "STATUS")) {
        print_status();
        return;
    }

    if (str_ieq(tok[0], "GET")) {
        print_staged();
        return;
    }

    if (str_ieq(tok[0], "APPLY")) {
        dsp_apply_remote_config(&g_staged);
        print_ok("APPLY_QUEUED");
        return;
    }

    if (str_ieq(tok[0], "SET")) {
        if (ntok < 3) {
            print_err("Usage: SET <MODE|RATE|WINDOW|WINDOW_SIZE|STALTA|GAIN> <value>");
            return;
        }

        if (str_ieq(tok[1], "MODE")) {
            uint8_t v;
            if (!parse_fsm_mode(tok[2], &v)) {
                print_err("Invalid mode. Use IDLE, ROTATING, STRUCTURAL, SEISMIC or 0..3.");
                return;
            }
            g_staged.target_mode = v;
            printf("OK STAGED MODE=%s\n", fsm_mode_name(v));
            return;
        }

        if (str_ieq(tok[1], "RATE")) {
            float v;
            if (!parse_float_strict(tok[2], &v) || v < 4.0f || v > 1000.0f) {
                print_err("Invalid rate. Use 4..1000 Hz.");
                return;
            }
            g_staged.sample_rate_hz = v;
            printf("OK STAGED RATE_HZ=%.2f\n", (double)v);
            return;
        }

        if (str_ieq(tok[1], "WINDOW")) {
            uint8_t v;
            if (!parse_window(tok[2], &v)) {
                print_err("Invalid window. Use RECT, HANN, HAMMING, FLATTOP, BLACKMAN or 0..4.");
                return;
            }
            g_staged.window_type = v;
            printf("OK STAGED WINDOW=%s\n", window_name(v));
            return;
        }

        if (str_ieq(tok[1], "WINDOW_SIZE") || str_ieq(tok[1], "SIZE")) {
            char *endp = NULL;
            long v = strtol(tok[2], &endp, 10);
            if (endp == tok[2] || *endp != '\0' || !(v == 128 || v == 256 || v == 512)) {
                print_err("Invalid window size. Use 128, 256 or 512.");
                return;
            }
            g_staged.window_size = (uint16_t)v;
            printf("OK STAGED WINDOW_SIZE=%u\n", (unsigned)g_staged.window_size);
            return;
        }

        if (str_ieq(tok[1], "STALTA")) {
            float v;
            if (!parse_float_strict(tok[2], &v) || v <= 1.0f) {
                print_err("Invalid STA/LTA threshold. Use value > 1.0.");
                return;
            }
            g_staged.stalta_thresh = v;
            printf("OK STAGED STALTA=%.3f\n", (double)v);
            return;
        }

        if (str_ieq(tok[1], "GAIN")) {
            float v;
            if (!parse_float_strict(tok[2], &v) || v <= 0.0f) {
                print_err("Invalid gain. Use value > 0.0.");
                return;
            }
            g_staged.calib_gain = v;
            printf("OK STAGED GAIN=%.3f\n", (double)v);
            return;
        }

        print_err("Unknown SET field.");
        return;
    }

    if (str_ieq(tok[0], "TELEMETRY") || str_ieq(tok[0], "TEL")) {
        if (ntok < 2) {
            print_err("Usage: TELEMETRY ON|OFF|ONCE|FAST|SLOW|PERIOD <ms>");
            return;
        }

        if (str_ieq(tok[1], "ON")) {
            g_telemetry_stream = true;
            g_last_telemetry_tx_ms = 0;
            printf("OK TELEMETRY=ON PERIOD_MS=%u\n", (unsigned)g_telemetry_period_ms);
            return;
        }

        if (str_ieq(tok[1], "OFF")) {
            g_telemetry_stream = false;
            g_telemetry_once = false;
            print_ok("TELEMETRY=OFF");
            return;
        }

        if (str_ieq(tok[1], "ONCE")) {
            g_telemetry_once = true;
            print_ok("TELEMETRY=ONCE");
            return;
        }

        if (str_ieq(tok[1], "FAST")) {
            g_telemetry_period_ms = 250;
            printf("OK TELEMETRY_PERIOD_MS=%u\n", (unsigned)g_telemetry_period_ms);
            return;
        }

        if (str_ieq(tok[1], "SLOW")) {
            g_telemetry_period_ms = 1000;
            printf("OK TELEMETRY_PERIOD_MS=%u\n", (unsigned)g_telemetry_period_ms);
            return;
        }

        if (str_ieq(tok[1], "PERIOD")) {
            if (ntok < 3) {
                print_err("Usage: TELEMETRY PERIOD <ms>");
                return;
            }

            char *endp = NULL;
            long v = strtol(tok[2], &endp, 10);
            if (endp == tok[2] || *endp != '\0' || v < 100 || v > 10000) {
                print_err("Invalid telemetry period. Use 100..10000 ms.");
                return;
            }

            g_telemetry_period_ms = (uint32_t)v;
            printf("OK TELEMETRY_PERIOD_MS=%u\n", (unsigned)g_telemetry_period_ms);
            return;
        }

        print_err("Usage: TELEMETRY ON|OFF|ONCE|FAST|SLOW|PERIOD <ms>");
        return;
    }

    if (str_ieq(tok[0], "FFT")) {
        if (ntok >= 2 && str_ieq(tok[1], "ONCE")) {
            g_fft_once = true;
            print_ok("FFT_ONCE");
            return;
        }
        print_err("Usage: FFT ONCE");
        return;
    }

    if (str_ieq(tok[0], "SIMULATE") || str_ieq(tok[0], "SIM")) {
        if (ntok < 2) {
            print_err("Usage: SIMULATE ON|OFF");
            return;
        }
        if (str_ieq(tok[1], "ON")) {
            g_simulate_mode = true;
            print_ok("SIMULATE=ON");
            return;
        }
        if (str_ieq(tok[1], "OFF")) {
            g_simulate_mode = false;
            print_ok("SIMULATE=OFF");
            return;
        }
        print_err("Usage: SIMULATE ON|OFF");
        return;
    }

    if (str_ieq(tok[0], "PING")) {
        printf("PONG UPTIME_MS=%u\n", (unsigned)to_ms_since_boot(get_absolute_time()));
        return;
    }

    if (str_ieq(tok[0], "ACQ")) {
        if (ntok >= 2 && (str_ieq(tok[1], "POLLING") || str_ieq(tok[1], "RESET"))) {
            if (main_restart_polling_acquisition()) {
                print_ok("ACQ=POLLING_RESTARTED");
            } else {
                print_err("ACQ restart rejected. Check MPU, IDLE mode or SIMULATE mode.");
            }
            return;
        }
        if (ntok >= 2 && str_ieq(tok[1], "DRDY")) {
            print_err("DRDY disabled in polling baseline. Use ACQ POLLING.");
            return;
        }
        print_err("Usage: ACQ POLLING");
        return;
    }

    if (str_ieq(tok[0], "DTC")) {
        if (ntok >= 2 && str_ieq(tok[1], "CLEAR")) {
            diagnostics_clear_all();
            print_ok("DTC_CLEAR");
            return;
        }
        printf("DTC ACTIVE=0x%04X COUNT=%u\n", bite_get_current_status(), (unsigned)diagnostics_get_count());
        return;
    }

    if (str_ieq(tok[0], "NET")) {
        if (ntok >= 2 && str_ieq(tok[1], "WIFI")) {
            if (ntok >= 3 && str_ieq(tok[2], "ON")) {
                bool ok = edge_net_set_enabled(true);
                printf("%s NET_WIFI=ON STATE=%s\n", ok ? "OK" : "ERR", net_state_name(edge_net_get_state()));
                return;
            }
            if (ntok >= 3 && str_ieq(tok[2], "OFF")) {
                bool ok = edge_net_set_enabled(false);
                printf("%s NET_WIFI=OFF STATE=%s\n", ok ? "OK" : "ERR", net_state_name(edge_net_get_state()));
                return;
            }
            if (ntok >= 3 && str_ieq(tok[2], "STATUS")) {
                printf("NET WIFI=%s STATE=%s\n", edge_net_is_enabled() ? "ON" : "OFF", net_state_name(edge_net_get_state()));
                return;
            }
            print_err("Usage: NET WIFI ON|OFF|STATUS");
            return;
        }

        printf("NET STATE=%s WIFI=%s\n", net_state_name(edge_net_get_state()), edge_net_is_enabled() ? "ON" : "OFF");
        return;
    }

    if (str_ieq(tok[0], "VERSION")) {
        printf("VERSION PROTOCOL=%u NODE_UUID=0x%04X\n",
               (unsigned)NET_PROTOCOL_VERSION,
               (unsigned)MY_NODE_UUID);
        return;
    }

    if (str_ieq(tok[0], "RESET")) {
        print_ok("RESET");
        sleep_ms(100);
        watchdog_reboot(0, 0, 0);
        return;
    }

    print_err("Unknown command. Use HELP.");
}

void serial_console_init(void) {
    g_linelen = 0;
    g_telemetry_stream = false;
    g_telemetry_once = false;
    g_simulate_mode = false;
    g_prompt_printed = false;
    g_fft_once = false;
    g_telemetry_period_ms = 1000;
    g_last_telemetry_tx_ms = 0;
    seed_staged_from_live();
}

static void maybe_print_welcome(void) {
    static bool was_connected = false;
    static bool boot_welcome_done = false;
    static uint32_t boot_ms = 0;

    if (boot_ms == 0) {
        boot_ms = to_ms_since_boot(get_absolute_time());
    }

    bool is_connected = stdio_usb_connected();

    if (is_connected && !was_connected) {
        printf("\nEDGE DSP VIBRATION NODE\n");
        printf("ASCII serial console ready. Type HELP and press Enter.\n");
        seed_staged_from_live();
        print_menu();
        serial_print_prompt();
        boot_welcome_done = true;
    }

    was_connected = is_connected;

    if (!boot_welcome_done &&
        (to_ms_since_boot(get_absolute_time()) - boot_ms) > BOOT_WELCOME_TIMEOUT_MS) {
        printf("\nEDGE DSP VIBRATION NODE\n");
        printf("ASCII serial console ready. Type HELP and press Enter.\n");
        print_menu();
        serial_print_prompt();
        boot_welcome_done = true;
    }
}

static void serial_handle_rx_char(int c) {
    if (c == PICO_ERROR_TIMEOUT) {
        return;
    }

    // Atalhos de emergência para recuperar controle do terminal durante stream.
    // Ctrl+C (0x03) ou '!' desligam a telemetria sem exigir Enter.
    if (c == 0x03 || c == '!') {
        g_telemetry_stream = false;
        g_telemetry_once = false;
        g_linelen = 0;
        printf("\r\nOK TELEMETRY=OFF\n");
        serial_print_prompt();
        return;
    }

    // Treat both CR and LF as line terminators. This fixes terminals that send
    // only '\r' when Enter is pressed.
    if (c == '\r' || c == '\n') {
        printf("\r\n");

        if (g_linelen > 0) {
            g_linebuf[g_linelen] = '\0';
            process_line(g_linebuf);
            g_linelen = 0;
        }

        serial_print_prompt();
        return;
    }

    // Backspace or DEL.
    if (c == 0x08 || c == 0x7F) {
        if (g_linelen > 0) {
            g_linelen--;
            printf("\b \b");
            fflush(stdout);
        }
        return;
    }

    // Ignore non-printable control characters.
    if (c < 0x20 || c > 0x7E) {
        return;
    }

    if (g_linelen < (LINE_BUF_SIZE - 1)) {
        g_linebuf[g_linelen++] = (char)c;
        putchar(c);       // local echo generated by the firmware
        fflush(stdout);
    } else {
        print_err("Line too long. Buffer cleared.");
        g_linelen = 0;
        serial_print_prompt();
    }
}

void serial_console_poll(void) {
    maybe_print_welcome();

    // Limit work per call so the console cannot monopolize the main loop.
    for (int guard = 0; guard < 64; guard++) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT) {
            break;
        }
        serial_handle_rx_char(c);
    }
}

bool serial_console_is_simulate_mode(void) {
    return g_simulate_mode;
}

bool serial_console_wants_telemetry(void) {
    return g_telemetry_stream || g_telemetry_once;
}

bool serial_console_wants_fft_once(void) {
    return g_fft_once;
}

void serial_console_report_telemetry(const DSP_AnalysisResult *res, OperationModeFSM mode, uint16_t dtc_code,
                                     AcquisitionMode acq_mode, BatteryStatus batt, uint8_t dtc_count) {
    if (!g_telemetry_stream && !g_telemetry_once) {
        return;
    }

    bool once = g_telemetry_once;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (!once && g_last_telemetry_tx_ms != 0 &&
        (now_ms - g_last_telemetry_tx_ms) < g_telemetry_period_ms) {
        return;
    }

    g_telemetry_once = false;
    g_last_telemetry_tx_ms = now_ms;

    printf("\r\nTEL MODE=%s ACQ=%s WIN=%u AXIS=VECTOR FFT_VALID=%s "
           "RMS=%.5f KURT=%.5f CREST=%.4f PEAK_HZ=%.3f "
           "PEAK_AMP=%.6f ENT=%.4f PPV_MM_S=%.4f STA_LTA=%s CLIP=%s "
           "BATT_PCT=%u BATT_MV=%u DTC=0x%04X DTC_COUNT=%u\n",
           fsm_mode_name((uint8_t)mode),
           acq_mode_name((uint8_t)acq_mode),
           (unsigned)res->window_size,
           yes_no(res->fft_valid),
           (double)res->rms_ac,
           (double)res->kurtosis,
           (double)res->crest_factor,
           (double)res->peak_freq_hz,
           (double)res->peak_amplitude,
           (double)res->spectral_entropy,
           (double)res->ppv_max_mm_s,
           yes_no(res->seismic_triggered),
           yes_no(res->clipping_detected),
           (unsigned)batt.pct,
           (unsigned)batt.mv,
           dtc_code,
           (unsigned)dtc_count);

    if (!g_telemetry_stream) {
        serial_print_prompt();
    }
}

void serial_console_report_config_applied(const Payload_Configuration *cfg, float effective_rate_hz,
                                          uint16_t effective_window_size, AcquisitionMode acq_mode) {
    if (!cfg) return;
    printf("\r\nCONFIG_APPLIED MODE=%s RATE_REQ=%.2f RATE_EFF=%.2f WINDOW_REQ=%u WINDOW_EFF=%u ACQ=%s\n",
           fsm_mode_name(cfg->target_mode),
           (double)cfg->sample_rate_hz,
           (double)effective_rate_hz,
           (unsigned)cfg->window_size,
           (unsigned)effective_window_size,
           acq_mode_name((uint8_t)acq_mode));
    serial_print_prompt();
}

void serial_console_report_fft(const float *fft_mag, uint16_t bins, bool valid) {
    if (!g_fft_once) return;
    g_fft_once = false;

    if (!valid || !fft_mag || bins == 0) {
        printf("\r\nFFT VALID=NO BINS=0\n");
        serial_print_prompt();
        return;
    }

    printf("\r\nFFT VALID=YES BINS=%u VALUES=", (unsigned)bins);
    for (uint16_t i = 0; i < bins; i++) {
        printf("%s%.6f", (i == 0 ? "" : ","), (double)fft_mag[i]);
        watchdog_update();
    }
    printf("\n");
    serial_print_prompt();
}

void serial_console_report_dtc(const DTC_Record *rec) {
    printf("\r\nDTC_EVENT CODE=0x%04X SYMPTOM=0x%02X SEVERITY=%u TS_MS=%u "
           "FSM=%u MPU_TEMP_C=%d MCU_TEMP_C=%d VCC_MV=%u\n",
           rec->dtc_code,
           rec->symptom,
           rec->severity,
           (unsigned)rec->freeze.timestamp_ms,
           (unsigned)rec->freeze.fsm_state,
           (int)rec->freeze.mpu_temp_c,
           (int)rec->freeze.mcu_temp_c,
           (unsigned)rec->freeze.vcc_mv);
    serial_print_prompt();
}

void serial_console_report_net_state(NetworkMode mode) {
    printf("\r\nNET_EVENT STATE=%s\n", net_state_name(mode));
    serial_print_prompt();
}
