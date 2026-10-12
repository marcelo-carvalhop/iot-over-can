/**
 * @file audio_app.c
 * @brief Ganchos de edge_app.h e telemetria do sensor acústico (perfil 0x02).
 */
#include "audio_app.h"

#include <math.h>
#include <string.h>

#include "hardware/sync.h"

#include "audio_config.h"
#include "battery_monitor.h"
#include "edge_diagnostics.h"
#include "edge_network_driver.h"

static Payload_Configuration g_current;          // em vigor (núcleo 0)
static float                 g_rate_eff = 0.0f;

// Pedidos do Node: escritos no contexto do lwIP, lidos pelo laço principal.
static Payload_Configuration g_pending_cfg;
static volatile bool         g_pending_cfg_valid = false;
static AudioPendingControl   g_pending_ctrl;

static uint8_t g_tx[EDGE_NET_MAX_PAYLOAD_BYTES];  // usado só pelo laço principal

void audio_app_init(const Payload_Configuration *initial) {
    if (initial) g_current = *initial;
    g_rate_eff = initial ? initial->sample_rate_hz : 0.0f;
    g_pending_cfg_valid = false;
    memset(&g_pending_ctrl, 0, sizeof(g_pending_ctrl));
}

const Payload_Configuration *audio_app_current_config(void) { return &g_current; }

void audio_app_set_current_config(const Payload_Configuration *cfg, float effective_rate_hz) {
    if (cfg) g_current = *cfg;
    if (effective_rate_hz > 0.0f) g_rate_eff = effective_rate_hz;
}

float audio_app_effective_rate_hz(void) { return g_rate_eff; }

/* ------------------------------------------------------------ edge_app.h */

uint8_t edge_app_profile_id(void) {
    return (uint8_t)PROFILE_ACOUSTIC_MEMS;
}

void edge_app_fill_config_menu(Payload_ConfigMenu *menu) {
    if (!menu) return;
    menu->num_fsm_modes = ACOUSTIC_MODE_EVENTS + 1;   // 0..3
    menu->num_window_types = 5;
    menu->min_sample_rate_hz = AUDIO_SAMPLE_RATES_HZ[0];
    menu->max_sample_rate_hz = AUDIO_SAMPLE_RATES_HZ[AUDIO_RATE_COUNT - 1];
    menu->min_buffer_size = AUDIO_FFT_SIZES[0];
    menu->max_buffer_size = AUDIO_FFT_SIZES[AUDIO_FFT_SIZE_COUNT - 1];
    menu->current_buffer_size = g_current.window_size;
    menu->current_fsm_mode = g_current.target_mode;
    menu->current_window_type = g_current.window_type;
    menu->current_sample_rate_hz = g_rate_eff;
    menu->current_stalta_thresh = g_current.stalta_thresh;
    menu->current_calib_gain = g_current.calib_gain;
}

uint16_t edge_app_effective_window_size(void) { return g_current.window_size; }
float    edge_app_effective_sample_rate_hz(void) { return g_rate_eff; }

void edge_app_queue_config(const Payload_Configuration *cfg) {
    if (!cfg) return;
    const uint32_t irq = save_and_disable_interrupts();
    g_pending_cfg = *cfg;
    g_pending_cfg_valid = true;
    restore_interrupts(irq);
}

bool edge_app_handle_session_command(uint8_t cmd_type, const uint8_t *inner, size_t len) {
    if (cmd_type != CMD_ACOUSTIC_CONTROL || !inner) return false;
    Payload_AcousticControl ctrl;
    if (len != sizeof(ctrl)) return false;  // malformado: o driver conta como recusa de protocolo
    memcpy(&ctrl, inner, sizeof(ctrl));
    const uint32_t irq = save_and_disable_interrupts();
    g_pending_ctrl.valid = true;
    g_pending_ctrl.request_counter = ctrl.request_counter;
    g_pending_ctrl.action = ctrl.action;
    g_pending_ctrl.argument = ctrl.argument;
    restore_interrupts(irq);
    return true;
}

bool audio_app_take_pending_config(Payload_Configuration *out) {
    bool taken = false;
    const uint32_t irq = save_and_disable_interrupts();
    if (g_pending_cfg_valid) {
        if (out) *out = g_pending_cfg;
        g_pending_cfg_valid = false;
        taken = true;
    }
    restore_interrupts(irq);
    return taken;
}

bool audio_app_take_pending_control(AudioPendingControl *out) {
    bool taken = false;
    const uint32_t irq = save_and_disable_interrupts();
    if (g_pending_ctrl.valid) {
        if (out) *out = g_pending_ctrl;
        g_pending_ctrl.valid = false;
        taken = true;
    }
    restore_interrupts(irq);
    return taken;
}

/* ------------------------------------------------------------- telemetria */

int16_t audio_db_to_cb(float db) {
    if (!(db == db)) return ACOUSTIC_CB_NA;
    float cb = db * 100.0f;
    if (cb > 32767.0f) cb = 32767.0f;
    if (cb < -32767.0f) cb = -32767.0f;
    return (int16_t)lroundf(cb);
}

static int16_t scaled_i16(float value, float scale) {
    if (!(value == value)) return ACOUSTIC_CB_NA;
    float v = value * scale;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32767.0f) v = -32767.0f;
    return (int16_t)lroundf(v);
}

static uint16_t scaled_u16(float value, float scale) {
    if (!(value == value) || value <= 0.0f) return 0;
    float v = value * scale;
    if (v > 65535.0f) v = 65535.0f;
    return (uint16_t)lroundf(v);
}

bool audio_app_send_report(const AudioReportMessage *msg, uint16_t seq) {
    if (!msg || edge_net_get_state() != NET_STATE_BOUND) return false;
    const AcousticReport *r = &msg->report;

    Payload_AcousticStream t;
    memset(&t, 0, sizeof(t));
    t.magic_header = NET_MAGIC_HEADER;
    t.cmd_type = CMD_ACOUSTIC_STREAM;
    t.seq_num = seq;
    t.mode = r->mode;
    t.flags = r->flags;
    t.window_type = g_current.window_type;
    t.fft_size = r->fft_size;
    t.sample_rate_req_hz = g_current.sample_rate_hz;
    t.sample_rate_eff_hz = r->sample_rate_hz;
    t.interval_ms = (uint16_t)lroundf(1000.0f * (float)r->samples / r->sample_rate_hz);
    t.coverage_pct = r->coverage_pct;
    t.cpu_pct = msg->cpu_pct;
    t.laeq_cb = audio_db_to_cb(r->laeq);
    t.lceq_cb = audio_db_to_cb(r->lceq);
    t.lzeq_cb = audio_db_to_cb(r->lzeq);
    t.lzpeak_cb = audio_db_to_cb(r->lzpeak);
    t.laf_cb = audio_db_to_cb(r->laf);
    t.lafmax_cb = audio_db_to_cb(r->lafmax);
    t.lafmin_cb = audio_db_to_cb(r->lafmin);
    t.crest_x100 = scaled_i16(r->crest, 100.0f);
    t.kurtosis_x100 = scaled_i16(r->kurtosis, 100.0f);
    t.peak_hz = r->peak_hz == r->peak_hz ? r->peak_hz : 0.0f;
    t.centroid_hz = scaled_u16(r->centroid_hz, 1.0f);
    t.flatness_x10000 = scaled_u16(r->flatness, 10000.0f);
    t.leq_total_cb = audio_db_to_cb(r->leq_total);
    t.l10_cb = audio_db_to_cb(r->l10);
    t.l50_cb = audio_db_to_cb(r->l50);
    t.l90_cb = audio_db_to_cb(r->l90);
    t.dose_nr15_x10 = scaled_u16(r->dose_nr15_pct, 10.0f);
    t.dose_nho01_x10 = scaled_u16(r->dose_nho01_pct, 10.0f);
    t.elapsed_s = r->elapsed_s;
    t.tonal_cb = audio_db_to_cb(r->tonal_db);
    t.f0_hz = r->f0_hz == r->f0_hz ? r->f0_hz : 0.0f;
    t.harmonics = r->harmonics;
    t.anomaly_x100 = scaled_i16(r->anomaly_z, 100.0f);
    t.baseline_pct = r->baseline_pct;
    t.event_count = r->event_count;
    t.events_interval = r->events_interval;
    t.event_class = r->event_class;
    t.event_peak_cb = audio_db_to_cb(r->event_peak_db);
    t.event_sel_cb = audio_db_to_cb(r->event_sel_db);
    t.event_duration_ms = r->event_duration_ms;
    t.event_centroid_hz = scaled_u16(r->event_centroid_hz, 1.0f);
    t.event_age_s = r->event_age_s;
    t.sta_lta_cb = audio_db_to_cb(r->sta_lta_db);
    t.background_cb = audio_db_to_cb(r->background_db);
    for (unsigned b = 0; b < ACOUSTIC_OCTAVE_BANDS; ++b) t.octave_cb[b] = audio_db_to_cb(r->octave_db[b]);
    t.battery_pct = battery_monitor_get_pct_for_beacon();
    t.battery_mv = battery_monitor_get_mv();
    t.dtc_count = diagnostics_get_count();
    t.dtc_active_code = diagnostics_get_active_code();

    // O espectro segue o relatório, como na telemetria de vibração; o Node
    // só o usa quando há um pedido de FFT pendente.
    const size_t base = sizeof(t);
    uint16_t bins = r->spectrum_bins;
    const uint16_t fit = (uint16_t)((EDGE_NET_MAX_PAYLOAD_BYTES - base) / sizeof(float));
    if (bins > fit) bins = fit;
    t.spectrum_bins = bins;
    memcpy(g_tx, &t, base);
    if (bins) memcpy(&g_tx[base], r->spectrum_pa, (size_t)bins * sizeof(float));
    return edge_net_send_data(g_tx, base + (size_t)bins * sizeof(float));
}

void audio_app_send_control_ack(uint32_t request_counter, uint8_t action, uint8_t status, uint16_t detail) {
    Payload_AcousticControlAck ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic_header = NET_MAGIC_HEADER;
    ack.cmd_type = CMD_ACOUSTIC_CONTROL_ACK;
    ack.request_counter = request_counter;
    ack.action = action;
    ack.status = status;
    ack.detail = detail;
    (void)edge_net_send_data(&ack, sizeof(ack));
}
