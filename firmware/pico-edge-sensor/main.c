/**
 * @file main.c
 * @brief Firmware principal do nó Edge DSP com serial, Wi-Fi/UDP, polling I2C e diagnóstico TUI-ready.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "hardware/adc.h"

#include "dsp_pipeline.h"
#include "mpu6050_dma_driver.h"
#include "edge_network_driver.h"
#include "serial_console.h"
#include "battery_monitor.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define WATCHDOG_TIMEOUT_MS 8000
#define DTC_QUEUE_CAPACITY 8
#define DTC_TABLE_CAPACITY 8
#define DTC_DUPLICATE_SUPPRESS_MS 2000

typedef struct {
    bool active;
    DTC_Record rec;
} DTC_TableEntry;

static OperationModeFSM g_active_fsm = FSM_MODE_ROTATING_MACH;
static bool             g_mpu_present = false;
static AcquisitionMode  g_acq_mode = ACQ_MODE_UNKNOWN;
static uint16_t         g_active_dtc = DTC_NONE;
static float            g_requested_sample_rate_hz = 1000.0f;
static int8_t           g_env_mpu_temp_c = 0;
static int8_t           g_env_mcu_temp_c = 0;
static uint16_t         g_env_vcc_mv = 0xFFFF;

static float work_x[DSP_BUFFER_MAX_SAMPLES];
static float work_y[DSP_BUFFER_MAX_SAMPLES];
static float work_z[DSP_BUFFER_MAX_SAMPLES];
static float fft_mag_out[DSP_BUFFER_MAX_SAMPLES / 2];

static volatile DTC_Record g_dtc_queue[DTC_QUEUE_CAPACITY];
static volatile uint8_t    g_dtc_queue_head = 0;
static volatile uint8_t    g_dtc_queue_tail = 0;
static volatile uint8_t    g_dtc_queue_count = 0;

static DTC_TableEntry g_dtc_table[DTC_TABLE_CAPACITY];
static uint16_t g_last_queued_dtc_code = DTC_NONE;
static uint8_t  g_last_queued_dtc_symptom = 0;
static uint8_t  g_last_queued_dtc_severity = 0;
static uint32_t g_last_queued_dtc_ms = 0;

static uint32_t g_prng_state = 0xC0FFEEu;

static float simulate_prng_unit(void) {
    g_prng_state ^= g_prng_state << 13;
    g_prng_state ^= g_prng_state >> 17;
    g_prng_state ^= g_prng_state << 5;
    return ((float)(g_prng_state & 0xFFFFu) / 65535.0f) * 2.0f - 1.0f;
}

static int8_t clamp_temp_to_i8(float t) {
    if (t > 127.0f) return 127;
    if (t < -128.0f) return -128;
    return (int8_t)lroundf(t);
}

static int8_t read_mcu_temp_c_i8(void) {
    adc_select_input(4);
    uint16_t raw = adc_read();
    const float conversion_factor = 3.3f / (1u << 12);
    float voltage = (float)raw * conversion_factor;
    float temp_c = 27.0f - (voltage - 0.706f) / 0.001721f;
    return clamp_temp_to_i8(temp_c);
}

static DTC_FreezeFrame make_freeze_frame(uint16_t code) {
    (void)code;
    DTC_FreezeFrame fr;
    fr.timestamp_ms = to_ms_since_boot(get_absolute_time());
    fr.fsm_state = (uint8_t)g_active_fsm;
    // Usa o último snapshot ambiental coletado no loop principal. Isso evita
    // acessar I2C/ADC de dentro de caminhos de diagnóstico que podem ter sido
    // acionados por erro de I2C ou por callback de rede.
    fr.mpu_temp_c = g_env_mpu_temp_c;
    fr.mcu_temp_c = g_env_mcu_temp_c;
    fr.vcc_mv = g_env_vcc_mv;
    return fr;
}

static void update_environment_snapshot(void) {
    float mpu_temp = 0.0f;
    if (g_mpu_present && mpu6050_read_temperature_c(&mpu_temp)) {
        g_env_mpu_temp_c = clamp_temp_to_i8(mpu_temp);
    }
    g_env_mcu_temp_c = read_mcu_temp_c_i8();
    g_env_vcc_mv = battery_monitor_get_mv();
}

static void update_active_dtc_summary(void) {
    uint16_t best = DTC_NONE;
    uint8_t best_sev = 0;
    uint32_t best_ts = 0;

    for (int i = 0; i < DTC_TABLE_CAPACITY; i++) {
        if (!g_dtc_table[i].active) continue;
        DTC_Record *r = &g_dtc_table[i].rec;
        if (r->severity > best_sev ||
            (r->severity == best_sev && r->freeze.timestamp_ms >= best_ts)) {
            best = r->dtc_code;
            best_sev = r->severity;
            best_ts = r->freeze.timestamp_ms;
        }
    }
    g_active_dtc = best;
}

static void diagnostics_store_record(const DTC_Record *rec) {
    int free_slot = -1;
    int oldest_slot = 0;
    uint32_t oldest_ts = UINT32_MAX;

    for (int i = 0; i < DTC_TABLE_CAPACITY; i++) {
        if (g_dtc_table[i].active && g_dtc_table[i].rec.dtc_code == rec->dtc_code) {
            g_dtc_table[i].rec = *rec;
            update_active_dtc_summary();
            return;
        }
        if (!g_dtc_table[i].active && free_slot < 0) free_slot = i;
        if (g_dtc_table[i].active && g_dtc_table[i].rec.freeze.timestamp_ms < oldest_ts) {
            oldest_ts = g_dtc_table[i].rec.freeze.timestamp_ms;
            oldest_slot = i;
        }
    }

    int slot = (free_slot >= 0) ? free_slot : oldest_slot;
    g_dtc_table[slot].active = true;
    g_dtc_table[slot].rec = *rec;
    update_active_dtc_summary();
}

static bool diagnostics_has_same_active_record(uint16_t code, uint8_t symptom, uint8_t severity) {
    for (int i = 0; i < DTC_TABLE_CAPACITY; i++) {
        if (g_dtc_table[i].active &&
            g_dtc_table[i].rec.dtc_code == code &&
            g_dtc_table[i].rec.symptom == symptom &&
            g_dtc_table[i].rec.severity == severity) {
            return true;
        }
    }
    return false;
}


void diagnostics_clear_all(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    memset(g_dtc_table, 0, sizeof(g_dtc_table));
    g_active_dtc = DTC_NONE;
    g_dtc_queue_head = g_dtc_queue_tail = g_dtc_queue_count = 0;
    g_last_queued_dtc_code = DTC_NONE;
    restore_interrupts(irq_state);
}

uint8_t diagnostics_get_count(void) {
    uint8_t n = 0;
    for (int i = 0; i < DTC_TABLE_CAPACITY; i++) if (g_dtc_table[i].active) n++;
    return n;
}

uint16_t diagnostics_get_active_code(void) { return g_active_dtc; }
uint16_t bite_get_current_status(void) { return g_active_dtc; }
OperationModeFSM main_get_active_fsm_mode(void) { return g_active_fsm; }
bool main_is_mpu_present(void) { return g_mpu_present; }
AcquisitionMode main_get_acquisition_mode(void) { return g_acq_mode; }
float main_get_requested_sample_rate_hz(void) { return g_requested_sample_rate_hz; }

bool main_restart_polling_acquisition(void) {
    if (!g_mpu_present || g_active_fsm == FSM_MODE_IDLE || serial_console_is_simulate_mode()) {
        return false;
    }

    mpu6050_set_buffer_size(dsp_get_active_window_size());
    mpu6050_start_polling_acquisition((uint32_t)g_requested_sample_rate_hz);
    g_acq_mode = ACQ_MODE_POLLING;
    return true;
}


void diag_report_dtc_event(uint16_t code, uint8_t symptom, uint8_t severity) {
    DTC_Record rec;
    rec.dtc_code = code;
    rec.symptom = symptom;
    rec.severity = severity;
    rec.freeze = make_freeze_frame(code);

    // Event-on-change:
    // se o mesmo DTC já está ativo com o mesmo sintoma e severidade, apenas
    // atualiza a tabela interna. Não reenfileira DTC_EVENT assíncrono a cada
    // tentativa de leitura I2C, senão a serial fica inutilizável.
    bool already_active_same = diagnostics_has_same_active_record(code, symptom, severity);
    diagnostics_store_record(&rec);

    if (already_active_same) {
        return;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    bool duplicate_recent =
        (g_last_queued_dtc_code == code) &&
        (g_last_queued_dtc_symptom == symptom) &&
        (g_last_queued_dtc_severity == severity) &&
        ((rec.freeze.timestamp_ms - g_last_queued_dtc_ms) < DTC_DUPLICATE_SUPPRESS_MS);

    if (!duplicate_recent) {
        if (g_dtc_queue_count >= DTC_QUEUE_CAPACITY) {
            g_dtc_queue_tail = (g_dtc_queue_tail + 1) % DTC_QUEUE_CAPACITY;
            g_dtc_queue_count--;
        }
        g_dtc_queue[g_dtc_queue_head] = rec;
        g_dtc_queue_head = (g_dtc_queue_head + 1) % DTC_QUEUE_CAPACITY;
        g_dtc_queue_count++;
        g_last_queued_dtc_code = code;
        g_last_queued_dtc_symptom = symptom;
        g_last_queued_dtc_severity = severity;
        g_last_queued_dtc_ms = rec.freeze.timestamp_ms;
    }
    restore_interrupts(irq_state);
}

static void diag_pump_pending_dtc(void) {
    while (true) {
        DTC_Record rec;
        uint32_t irq_state = save_and_disable_interrupts();
        if (g_dtc_queue_count == 0) {
            restore_interrupts(irq_state);
            break;
        }
        rec = (DTC_Record)g_dtc_queue[g_dtc_queue_tail];
        g_dtc_queue_tail = (g_dtc_queue_tail + 1) % DTC_QUEUE_CAPACITY;
        g_dtc_queue_count--;
        restore_interrupts(irq_state);

        edge_net_send_urgent_dtc(&rec);
        serial_console_report_dtc(&rec);
    }
}

static uint8_t dtc_code_to_beacon_hint(uint16_t code) {
    if (code == DTC_NONE) return 0x00;
    uint8_t category_nibble = (uint8_t)((code >> 12) & 0x0F);
    uint8_t low_nibble = (uint8_t)(code & 0x0F);
    return (uint8_t)((category_nibble << 4) | low_nibble);
}

static void check_gravitational_sanity(const float *x, const float *y, const float *z,
                                       uint16_t n, bool seismic_already_triggered) {
    if (!x || !y || !z || n == 0) return;
    float sum = 0.0f;
    for (uint16_t i = 0; i < n; i++) {
        sum += sqrtf(x[i] * x[i] + y[i] * y[i] + z[i] * z[i]);
    }
    float mean_g = fabsf(sum / (float)n);
    if ((mean_g < 8.0f || mean_g > 11.5f) && !seismic_already_triggered) {
        diag_report_dtc_event(DTC_SENS_GRAVITY, FTB_OUT_OF_RANGE_LOW, SEV_CRITICAL);
    }
}

static bool simulate_maybe_generate_buffer(float *out_x, float *out_y, float *out_z,
                                           float fs_hz, uint32_t now_ms, bool *out_clipping) {
    static uint32_t last_ms = 0;
    uint16_t n = dsp_get_active_window_size();
    uint32_t interval_ms = (uint32_t)(((float)n / fs_hz) * 1000.0f);
    if (interval_ms == 0) interval_ms = 1;
    if (now_ms - last_ms < interval_ms) return false;
    last_ms = now_ms;

    const float sim_freq_hz = 50.0f;
    for (uint16_t i = 0; i < n; i++) {
        float t = (float)i / fs_hz;
        out_z[i] = 9.81f + 2.0f * sinf(2.0f * (float)M_PI * sim_freq_hz * t)
                         + 0.15f * simulate_prng_unit();
        out_x[i] = 0.15f * sinf(2.0f * (float)M_PI * 17.0f * t) + 0.05f * simulate_prng_unit();
        out_y[i] = 0.10f * sinf(2.0f * (float)M_PI * 23.0f * t) + 0.05f * simulate_prng_unit();
    }
    *out_clipping = false;
    return true;
}

int main(void) {
    stdio_init_all();
    adc_init();
    adc_set_temp_sensor_enabled(true);

    dsp_init_engine();
    mpu6050_init_dma_driver();
    battery_monitor_init();
    edge_net_init();
    serial_console_init();

    g_mpu_present = mpu6050_check_who_am_i();
    if (!g_mpu_present) {
        g_acq_mode = ACQ_MODE_UNKNOWN;
        diag_report_dtc_event(DTC_I2C_MPU_COMM, FTB_SIGNAL_OPEN, SEV_CRITICAL);
    } else {
        g_requested_sample_rate_hz = 1000.0f;
        mpu6050_set_buffer_size(dsp_get_active_window_size());
        mpu6050_start_polling_acquisition((uint32_t)g_requested_sample_rate_hz);
        g_acq_mode = ACQ_MODE_POLLING;
    }

    update_environment_snapshot();

    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);

    uint32_t last_beacon_ms = to_ms_since_boot(get_absolute_time());
    NetworkMode last_net_state = edge_net_get_state();
    uint32_t last_i2c_fail_warn_ms = 0;
    uint32_t last_env_snapshot_ms = 0;

    while (true) {
        watchdog_update();
        serial_console_poll();
        battery_monitor_poll();
        diag_pump_pending_dtc();
        edge_net_poll_timeout();

        NetworkMode cur_net_state = edge_net_get_state();
        if (cur_net_state != last_net_state) {
            serial_console_report_net_state(cur_net_state);
            last_net_state = cur_net_state;
        }

        Payload_Configuration applied_cfg;
        if (dsp_apply_pending_config(&applied_cfg)) {
            if (applied_cfg.target_mode <= FSM_MODE_SEISMIC_STALTA) {
                g_active_fsm = (OperationModeFSM)applied_cfg.target_mode;
            }

            if (applied_cfg.sample_rate_hz >= 1.0f) {
                g_requested_sample_rate_hz = applied_cfg.sample_rate_hz;
            }

            mpu6050_set_buffer_size(dsp_get_active_window_size());

            if (g_active_fsm == FSM_MODE_IDLE) {
                if (g_mpu_present) mpu6050_stop_acquisition();
                g_acq_mode = ACQ_MODE_IDLE;
            } else if (g_mpu_present) {
                // Baseline polling-only: não tenta religar DRDY.
                mpu6050_start_polling_acquisition((uint32_t)g_requested_sample_rate_hz);
                g_acq_mode = ACQ_MODE_POLLING;
            }

            serial_console_report_config_applied(&applied_cfg,
                                                 mpu6050_get_configured_sample_rate_hz(),
                                                 dsp_get_active_window_size(),
                                                 g_acq_mode);
            edge_net_send_config_ack(&applied_cfg, 1); // 1 = applied
        }

        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        if (last_env_snapshot_ms == 0 || now_ms - last_env_snapshot_ms >= 1000) {
            update_environment_snapshot();
            last_env_snapshot_ms = now_ms;
        }

        if (edge_net_get_state() == NET_STATE_DISCOVERY) {
            if (now_ms - last_beacon_ms >= 2000) {
                edge_net_send_discovery_beacon(dtc_code_to_beacon_hint(g_active_dtc));
                last_beacon_ms = now_ms;
            }
        }

        bool clipping = false;
        bool process_buffer = false;

        if (g_active_fsm == FSM_MODE_IDLE) {
            process_buffer = false;
        } else if (serial_console_is_simulate_mode()) {
            g_acq_mode = ACQ_MODE_SIMULATED;
            float fs_hz_sim = mpu6050_get_configured_sample_rate_hz();
            process_buffer = simulate_maybe_generate_buffer(work_x, work_y, work_z, fs_hz_sim, now_ms, &clipping);
        } else {
            bool telemetry_needed = serial_console_wants_telemetry() || serial_console_wants_fft_once() || (edge_net_get_state() == NET_STATE_BOUND);

            if (g_mpu_present && telemetry_needed) {
                g_acq_mode = ACQ_MODE_POLLING;
                process_buffer = mpu6050_get_polling_buffer(work_x, work_y, work_z, &clipping);

                if (!process_buffer && now_ms - last_i2c_fail_warn_ms >= 5000) {
                    printf("\r\nERR I2C_POLLING_READ_FAILED CHECK_SDA_SCL_PULLUPS_AND_MPU_POWER\nEDGE> ");
                    fflush(stdout);
                    last_i2c_fail_warn_ms = now_ms;
                }
            }
        }

        if (process_buffer) {
            if (clipping) {
                diag_report_dtc_event(DTC_SENS_CLIPPING, FTB_OUT_OF_RANGE_HIGH, SEV_WARNING);
            }

            bool seismic_was_triggered = dsp_is_seismic_currently_triggered();
            if (!serial_console_is_simulate_mode()) {
                check_gravitational_sanity(work_x, work_y, work_z,
                                           dsp_get_active_window_size(),
                                           seismic_was_triggered);
            }

            DSP_AnalysisResult dsp_res;
            dsp_run_vibration_pipeline(work_x, work_y, work_z,
                                       mpu6050_get_configured_sample_rate_hz(),
                                       g_active_fsm,
                                       clipping, &dsp_res, fft_mag_out);

            uint16_t fft_bins = dsp_res.fft_valid ? (dsp_res.window_size / 2) : 0;
            edge_net_send_telemetry(&dsp_res, g_active_fsm, g_active_dtc, fft_mag_out, fft_bins);
            serial_console_report_fft(fft_mag_out, fft_bins, dsp_res.fft_valid);
            serial_console_report_telemetry(&dsp_res, g_active_fsm, g_active_dtc,
                                            g_acq_mode,
                                            battery_monitor_get_status(),
                                            diagnostics_get_count());
        }

        sleep_ms(2);
    }
}
