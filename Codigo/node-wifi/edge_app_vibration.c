/**
 * @file edge_app_vibration.c
 * @brief Ganchos de edge_app.h e telemetria do sensor de vibração (perfil 0x01).
 */
#include "edge_app_vibration.h"

#include <string.h>

#include "battery_monitor.h"
#include "edge_diagnostics.h"
#include "edge_network_driver.h"
#include "mpu6050_dma_driver.h"

uint8_t edge_app_profile_id(void) {
    return (uint8_t)PROFILE_ACCELEROMETER;
}

void edge_app_fill_config_menu(Payload_ConfigMenu *menu) {
    if (!menu) return;
    menu->num_fsm_modes = FSM_MODE_SEISMIC_STALTA + 1;   // 0..3
    menu->num_window_types = WIN_BLACKMAN_H + 1;          // 0..4
    menu->min_sample_rate_hz = 4;
    menu->max_sample_rate_hz = 1000;
    menu->min_buffer_size = DSP_BUFFER_MIN_SAMPLES;
    menu->max_buffer_size = DSP_BUFFER_MAX_SAMPLES;
    menu->current_buffer_size = dsp_get_active_window_size();
    menu->current_fsm_mode = (uint8_t)main_get_active_fsm_mode();
    menu->current_window_type = (uint8_t)dsp_get_current_window();
    menu->current_sample_rate_hz = mpu6050_get_configured_sample_rate_hz();
    menu->current_stalta_thresh = dsp_get_stalta_threshold();
    menu->current_calib_gain = dsp_get_calibration_gain();
}

uint16_t edge_app_effective_window_size(void) {
    return dsp_get_active_window_size();
}

float edge_app_effective_sample_rate_hz(void) {
    return mpu6050_get_configured_sample_rate_hz();
}

void edge_app_queue_config(const Payload_Configuration *cfg) {
    dsp_apply_remote_config(cfg);
}

bool edge_app_handle_session_command(uint8_t cmd_type, const uint8_t *inner, size_t len) {
    // O sensor de vibração não tem comandos além dos do enlace.
    (void)cmd_type;
    (void)inner;
    (void)len;
    return false;
}

// Usado apenas pelo laço principal.
static uint8_t g_tx_inner[EDGE_NET_MAX_PAYLOAD_BYTES];

void edge_net_send_telemetry(const DSP_AnalysisResult *res, OperationModeFSM mode,
                             uint16_t dtc_code, const float *fft_mag, uint16_t fft_bins) {
    if (edge_net_get_state() != NET_STATE_BOUND || !res) return;

    const uint32_t base_sz = sizeof(Payload_TelemetryStream);

    // Item 5: limita quantos bins de FFT são anexados para não ultrapassar
    // NET_MAX_SAFE_PAYLOAD_BYTES (já descontado o envelope) e cair em
    // fragmentação IP. Se o chamador pedir mais do que cabe, trunca.
    uint16_t max_bins_that_fit = (uint16_t)((EDGE_NET_MAX_PAYLOAD_BYTES - base_sz) / sizeof(float));
    uint16_t bins_to_send = (fft_mag != NULL && res->fft_valid) ? fft_bins : 0;
    if (bins_to_send > max_bins_that_fit) bins_to_send = max_bins_that_fit;
    const uint32_t arr_sz = (uint32_t)bins_to_send * sizeof(float);

    Payload_TelemetryStream t;
    memset(&t, 0, sizeof(t));
    t.magic_header = NET_MAGIC_HEADER;
    t.cmd_type = CMD_TELEMETRY_STREAM;
    t.seq_num = edge_net_next_seq();
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

    memcpy(g_tx_inner, &t, base_sz);
    if (arr_sz > 0) memcpy(&g_tx_inner[base_sz], fft_mag, arr_sz);
    (void)edge_net_send_data(g_tx_inner, base_sz + arr_sz);
}
