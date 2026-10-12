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
#include "config_validation.h"
#include "serial_console.h"
#include "battery_monitor.h"
#include "edge_app_vibration.h"
#include "edge_diagnostics.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define WATCHDOG_TIMEOUT_MS 8000

static OperationModeFSM g_active_fsm = FSM_MODE_ROTATING_MACH;
static bool             g_mpu_present = false;
static AcquisitionMode  g_acq_mode = ACQ_MODE_UNKNOWN;
static float            g_requested_sample_rate_hz = 1000.0f;
static int8_t           g_env_mpu_temp_c = 0;
static int8_t           g_env_mcu_temp_c = 0;
static uint16_t         g_env_vcc_mv = 0xFFFF;

static float work_x[DSP_BUFFER_MAX_SAMPLES];
static float work_y[DSP_BUFFER_MAX_SAMPLES];
static float work_z[DSP_BUFFER_MAX_SAMPLES];
static float fft_mag_out[DSP_BUFFER_MAX_SAMPLES / 2];

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

static void update_environment_snapshot(void) {
    float mpu_temp = 0.0f;
    if (g_mpu_present && mpu6050_read_temperature_c(&mpu_temp)) {
        g_env_mpu_temp_c = clamp_temp_to_i8(mpu_temp);
    }
    g_env_mcu_temp_c = read_mcu_temp_c_i8();
    g_env_vcc_mv = battery_monitor_get_mv();
    edge_diag_set_context((uint8_t)g_active_fsm, g_env_mpu_temp_c, g_env_mcu_temp_c, g_env_vcc_mv);
}

OperationModeFSM main_get_active_fsm_mode(void) { return g_active_fsm; }
bool main_is_mpu_present(void) { return g_mpu_present; }
AcquisitionMode main_get_acquisition_mode(void) { return g_acq_mode; }
float main_get_requested_sample_rate_hz(void) { return g_requested_sample_rate_hz; }

static bool main_rate_to_u32(float rate_hz, uint32_t *out_rate) {
    if (!out_rate || !edge_config_valid_sample_rate(rate_hz)) return false;
    *out_rate = (uint32_t)rate_hz;
    return true;
}

bool main_restart_polling_acquisition(void) {
    if (!g_mpu_present || g_active_fsm == FSM_MODE_IDLE) {
        return false;
    }

    uint32_t rate_hz = 0;
    if (!main_rate_to_u32(g_requested_sample_rate_hz, &rate_hz)) return false;
    mpu6050_set_buffer_size(dsp_get_active_window_size());
    mpu6050_start_polling_acquisition(rate_hz);
    g_acq_mode = ACQ_MODE_POLLING;
    return true;
}


// DTCs registrados pelos drivers (edge_diagnostics.c) seguem para o Node e
// para o console a partir do laço principal, nunca de uma ISR.
static void diag_pump_pending_dtc(void) {
    DTC_Record rec;
    while (edge_diag_pop_pending(&rec)) {
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

int main(void) {
    stdio_init_all();
    adc_init();
    adc_set_temp_sensor_enabled(true);

    edge_diag_init();
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
        uint32_t boot_rate_hz = 1000u;
        mpu6050_set_buffer_size(dsp_get_active_window_size());
        mpu6050_start_polling_acquisition(boot_rate_hz);
        g_acq_mode = ACQ_MODE_POLLING;
    }

    update_environment_snapshot();

    watchdog_enable(WATCHDOG_TIMEOUT_MS, true);

    uint32_t last_beacon_ms = to_ms_since_boot(get_absolute_time());
    uint32_t next_beacon_delay_ms = edge_net_next_discovery_delay_ms();
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
            EdgeConfigValidationError cfg_error;
            uint32_t requested_rate_u32 = 0;
            if (!edge_config_validate(&applied_cfg, &cfg_error) ||
                !main_rate_to_u32(applied_cfg.sample_rate_hz, &requested_rate_u32)) {
                diag_report_dtc_event(DTC_SYS_CONFIG_REJECT, FTB_OUT_OF_RANGE_HIGH, SEV_WARNING);
                edge_net_send_config_ack(&applied_cfg, 2);
            } else {
                g_active_fsm = (OperationModeFSM)applied_cfg.target_mode;
                g_requested_sample_rate_hz = applied_cfg.sample_rate_hz;
                edge_diag_set_context((uint8_t)g_active_fsm, g_env_mpu_temp_c, g_env_mcu_temp_c, g_env_vcc_mv);
                mpu6050_set_buffer_size(dsp_get_active_window_size());

                if (g_active_fsm == FSM_MODE_IDLE) {
                    if (g_mpu_present) mpu6050_stop_acquisition();
                    g_acq_mode = ACQ_MODE_IDLE;
                } else if (g_mpu_present) {
                    // Aquisição em polling: não tenta religar DRDY.
                    mpu6050_start_polling_acquisition(requested_rate_u32);
                    g_acq_mode = ACQ_MODE_POLLING;
                }

                serial_console_report_config_applied(&applied_cfg,
                                                     mpu6050_get_configured_sample_rate_hz(),
                                                     dsp_get_active_window_size(),
                                                     g_acq_mode);
                edge_net_send_config_ack(&applied_cfg, 1);
            }
        }

        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        if (last_env_snapshot_ms == 0 || now_ms - last_env_snapshot_ms >= 1000) {
            update_environment_snapshot();
            last_env_snapshot_ms = now_ms;
        }

        if (edge_net_get_state() == NET_STATE_DISCOVERY) {
            if (now_ms - last_beacon_ms >= next_beacon_delay_ms) {
                edge_net_send_discovery_beacon(dtc_code_to_beacon_hint(diagnostics_get_active_code()));
                last_beacon_ms = now_ms;
                next_beacon_delay_ms = edge_net_next_discovery_delay_ms();
            }
        } else {
            last_beacon_ms = now_ms;
            next_beacon_delay_ms = edge_net_next_discovery_delay_ms();
        }

        bool clipping = false;
        bool process_buffer = false;

        if (g_active_fsm == FSM_MODE_IDLE) {
            process_buffer = false;
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
            check_gravitational_sanity(work_x, work_y, work_z,
                                       dsp_get_active_window_size(),
                                       seismic_was_triggered);

            DSP_AnalysisResult dsp_res;
            dsp_run_vibration_pipeline(work_x, work_y, work_z,
                                       mpu6050_get_configured_sample_rate_hz(),
                                       g_active_fsm,
                                       clipping, &dsp_res, fft_mag_out);

            uint16_t fft_bins = dsp_res.fft_valid ? (dsp_res.window_size / 2) : 0;
            edge_net_send_telemetry(&dsp_res, g_active_fsm, diagnostics_get_active_code(), fft_mag_out, fft_bins);
            serial_console_report_fft(fft_mag_out, fft_bins, dsp_res.fft_valid);
            serial_console_report_telemetry(&dsp_res, g_active_fsm, diagnostics_get_active_code(),
                                            g_acq_mode,
                                            battery_monitor_get_status(),
                                            diagnostics_get_count());
        }

        sleep_ms(2);
    }
}
