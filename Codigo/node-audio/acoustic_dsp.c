/**
 * @file acoustic_dsp.c
 * @brief Análise acústica do INMP441: níveis, ponderações, dose, assinatura e eventos.
 *
 * Veja a organização em acoustic_dsp.h. Referências usadas:
 *   - IEC 61672-1: ponderações A e C (expressões analíticas) e constantes de
 *     tempo F (125 ms) e S (1 s);
 *   - NR-15, Anexo 1: critério 85 dB(A), incremento de duplicação de dose
 *     q = 5 dB; NHO-01 (Fundacentro): critério 85 dB(A), q = 3 dB; ambas com
 *     limiar de integração de 80 dB(A) e 115 dB(A) como teto sem proteção;
 *   - ISO 266: frequências nominais das bandas de oitava.
 * As doses são indicativas: o INMP441 não é um dosímetro classe 2.
 */
#include "acoustic_dsp.h"

#include <math.h>
#include <string.h>

#include "edge_protocol_definitions.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

#define P0_SQ ((double)ACOUSTIC_P0_PA * (double)ACOUSTIC_P0_PA)

#define TAU_FAST_S      0.125
#define TAU_SLOW_S      1.0
#define TAU_STA_S       0.05
#define TAU_LTA_S       10.0
#define DC_ALPHA        0.01     // média do offset por subquadro (τ ≈ 1 s)
#define MIC_FAULT_SUB   100      // 1 s de palavras constantes
#define EVENT_WARMUP_S  2u       // o fundo (LTA) precisa convergir antes de armar
#define EVENT_MAX_S     30u      // evento mais longo: depois disso o fundo é refeito
#define EVENT_END_RATIO 2.0      // STA/LTA abaixo de +3 dB encerra o evento
#define EVENT_SHORT_MS  400u     // até aqui o evento é impulsivo
#define EVENT_CRACK_HZ  2500.0f  // centroide acima disso: estalo agudo
#define EVENT_TONAL_FLATNESS 0.1f
#define DOSE_CRITERION_DB  85.0
#define DOSE_THRESHOLD_DB  80.0
#define DOSE_DAY_S         28800.0  // jornada de 8 h
#define CEILING_DBA        115.0f   // NR-15: sem exposição acima disso sem proteção
#define HPS_HARMONICS      5
#define HARMONIC_MAX       10
#define HARMONIC_MIN_VALID 3
#define BASELINE_MIN_SD_DB 0.5

static const float BAND_NOMINAL_HZ[ACOUSTIC_BANDS] = {
    31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};

/* ----------------------------------------------------------------- utilitários */

static float db_power(double pa2) {
    const double r = pa2 / P0_SQ;
    return r > 1.0 ? (float)(10.0 * log10(r)) : 0.0f;
}

static float db_pressure(double pa) {
    const double r = pa / (double)ACOUSTIC_P0_PA;
    return r > 1.0 ? (float)(20.0 * log10(r)) : 0.0f;
}

float acoustic_band_center_hz(unsigned band) {
    return band < ACOUSTIC_BANDS ? BAND_NOMINAL_HZ[band] : 0.0f;
}

// Constantes de IEC 61672-1 (polos em Hz).
#define F1 20.598997
#define F2 107.65265
#define F3 737.86223
#define F4 12194.217

float acoustic_weight_a_db(float f_hz) {
    if (!(f_hz > 0.0f)) return -200.0f;
    const double f2 = (double)f_hz * (double)f_hz;
    const double ra = (F4 * F4 * f2 * f2) /
                      ((f2 + F1 * F1) * sqrt((f2 + F2 * F2) * (f2 + F3 * F3)) * (f2 + F4 * F4));
    return (float)(20.0 * log10(ra) + 2.0);
}

float acoustic_weight_c_db(float f_hz) {
    if (!(f_hz > 0.0f)) return -200.0f;
    const double f2 = (double)f_hz * (double)f_hz;
    const double rc = (F4 * F4 * f2) / ((f2 + F1 * F1) * (f2 + F4 * F4));
    return (float)(20.0 * log10(rc) + 0.062);
}

static float window_value(uint8_t type, uint32_t n, uint32_t size) {
    // Janelas periódicas (DFT-par), adequadas à análise espectral.
    const double x = 2.0 * M_PI * (double)n / (double)size;
    switch (type) {
        case 0: return 1.0f;
        case 1: return (float)(0.5 - 0.5 * cos(x));
        case 2: return (float)(0.54 - 0.46 * cos(x));
        case 3: return (float)(0.215578948 - 0.416631580 * cos(x) + 0.277263158 * cos(2.0 * x) -
                               0.083578947 * cos(3.0 * x) + 0.006947368 * cos(4.0 * x));
        case 4: return (float)(0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2.0 * x) - 0.01168 * cos(3.0 * x));
        default: return 1.0f;
    }
}

/* -------------------------------------------------------- estado e reinícios */

static void clear_baseline(AcousticEngine *e) {
    memset(e->base_n, 0, sizeof(e->base_n));
    memset(e->base_mean, 0, sizeof(e->base_mean));
    memset(e->base_m2, 0, sizeof(e->base_m2));
    e->learn_total_s = 0;
    e->learn_left_s = 0;
    e->baseline_ready = false;
}

static void clear_interval(AcousticEngine *e) {
    e->rep_energy_z = 0.0;
    e->rep_n = 0;
    e->rep_peak = 0;
    e->rep_overload = 0;
    e->rep_lost = 0;
    e->laf_max = -INFINITY;
    e->laf_min = INFINITY;
    e->las_max = -INFINITY;
    memset(e->welch, 0, sizeof(e->welch));
    e->welch_count = 0;
    e->rep_fft_ez = e->rep_fft_ea = e->rep_fft_ec = 0.0;
    e->rep_m2 = e->rep_m4 = 0.0;
    e->rep_kurt_n = 0;
    e->rep_frames_done = 0;
    e->rep_frames_fft = 0;
    e->ev_interval = 0;
}

// Reinício da medição (MEASURE_RESET): zera o que é acumulado desde o
// reinício, sem perder a continuidade das médias temporais e do fundo.
static void reset_measurement(AcousticEngine *e) {
    memset(e->ln_hist, 0, sizeof(e->ln_hist));
    e->ln_count = 0;
    e->dose_nr15 = 0.0;
    e->dose_nho01 = 0.0;
    e->energy_a_total = 0.0;
    e->elapsed_samples = 0.0;
    e->ev_active = false;
    e->ev_count = 0;
    e->ev_interval = 0;
    e->ev_any = false;
    e->ev_last_class = ACOUSTIC_EVENT_NONE;
    e->ev_last_peak_db = NAN;
    e->ev_last_sel_db = NAN;
    e->ev_last_centroid = NAN;
    e->ev_last_duration_ms = 0;
    e->ev_last_end_samples = 0.0;
}

void acoustic_engine_init(AcousticEngine *e) {
    if (!e) return;
    memset(e, 0, sizeof(*e));
    clear_baseline(e);
    clear_interval(e);
    reset_measurement(e);
}

bool acoustic_engine_configure(AcousticEngine *e, const AcousticConfig *cfg) {
    if (!e || !cfg) return false;
    if (cfg->mode > ACOUSTIC_MODE_EVENTS) return false;
    if (!(cfg->sample_rate_hz >= 7000.0f && cfg->sample_rate_hz <= 50000.0f)) return false;
    if (cfg->window_type > 4) return false;
    if (!(cfg->threshold > 1.0f && cfg->threshold <= 1000.0f)) return false;
    if (!(cfg->gain > 0.0f && cfg->gain <= 1000.0f)) return false;
    if (!audio_fft_init(&e->fft, cfg->fft_size)) return false;

    const bool same_bands = e->configured && e->cfg.fft_size == cfg->fft_size &&
                            fabsf(e->cfg.sample_rate_hz - cfg->sample_rate_hz) < 0.5f;
    e->cfg = *cfg;
    e->pa_per_count = ACOUSTIC_PA_PER_FULL_SCALE * cfg->gain / ACOUSTIC_FULL_SCALE_COUNTS;
    e->subframe_len = (uint32_t)lroundf(cfg->sample_rate_hz / (float)ACOUSTIC_SUBFRAMES_PER_S);
    if (e->subframe_len == 0) e->subframe_len = 1;
    e->report_len = e->subframe_len * ACOUSTIC_SUBFRAMES_PER_S;
    e->t_sub = (float)e->subframe_len / cfg->sample_rate_hz;
    e->alpha_fast = (float)(1.0 - exp(-(double)e->t_sub / TAU_FAST_S));
    e->alpha_slow = (float)(1.0 - exp(-(double)e->t_sub / TAU_SLOW_S));
    e->alpha_sta = (float)(1.0 - exp(-(double)e->t_sub / TAU_STA_S));
    e->alpha_lta = (float)(1.0 - exp(-(double)e->t_sub / TAU_LTA_S));

    const uint32_t n = cfg->fft_size;
    double sum_sq = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        e->window[i] = window_value(cfg->window_type, i, n);
        sum_sq += (double)e->window[i] * (double)e->window[i];
    }
    e->window_ms = (float)(sum_sq / (double)n);

    const uint32_t half = n / 2u;
    const float df = cfg->sample_rate_hz / (float)n;
    e->weight_a[0] = e->weight_c[0] = 0.0f;
    for (uint32_t k = 1; k <= half; ++k) {
        const float f = df * (float)k;
        e->weight_a[k] = (float)pow(10.0, (double)acoustic_weight_a_db(f) / 10.0);
        e->weight_c[k] = (float)pow(10.0, (double)acoustic_weight_c_db(f) / 10.0);
    }

    // Novo fluxo de amostras: tudo o que é integrado recomeça.
    e->sub_sum = e->sub_sumsq = 0;
    e->sub_peak = 0;
    e->sub_or = 0;
    e->sub_and = -1;
    e->sub_n = 0;
    e->dc = 0.0;
    e->dc_valid = false;
    e->tw_valid = false;
    e->ratio_valid = false;
    e->ratio_a = e->ratio_c = 1.0f;
    e->frame_fill = 0;
    e->frame_cur = 0;
    e->prev_pending = false;
    e->frames_since_fft = 1000;   // o primeiro quadro sempre passa pela FFT
    e->sl_valid = false;
    e->warmup_sub = 0;
    e->silent_sub = e->high_sub = 0;
    e->mic_state = ACOUSTIC_MIC_OK;
    e->report_ready = false;
    clear_interval(e);
    reset_measurement(e);
    if (!same_bands) clear_baseline(e);
    e->configured = true;
    return true;
}

bool acoustic_engine_control(AcousticEngine *e, uint8_t action, uint16_t argument) {
    if (!e || !e->configured) return false;
    switch (action) {
        case ACOUSTIC_CTRL_MEASURE_RESET:
            reset_measurement(e);
            return true;
        case ACOUSTIC_CTRL_BASELINE_LEARN:
            // A assinatura é a do modo MACHINE; de 5 s a 1 h de aprendizado.
            if (e->cfg.mode != ACOUSTIC_MODE_MACHINE || argument < 5u || argument > 3600u) return false;
            clear_baseline(e);
            e->learn_total_s = argument;
            e->learn_left_s = argument;
            return true;
        case ACOUSTIC_CTRL_BASELINE_CLEAR:
            clear_baseline(e);
            return true;
        default:
            return false;
    }
}

uint8_t acoustic_engine_mic_state(const AcousticEngine *e) {
    return e ? e->mic_state : ACOUSTIC_MIC_OK;
}

/* ---------------------------------------------------------- quadro da FFT */

static void analyze_frame(AcousticEngine *e, const int32_t *frame, bool event_frame) {
    const uint32_t n = e->cfg.fft_size;
    const uint32_t half = n / 2u;
    const float k = e->pa_per_count;
    const float dc = (float)e->dc;

    float m2 = 0.0f;
    float m4 = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const float x = ((float)frame[i] - dc) * k;
        const float x2 = x * x;
        m2 += x2;
        m4 += x2 * x2;
        e->work[i] = x * e->window[i];
    }
    audio_fft_power(&e->fft, e->work, e->power);

    // P_k = 2|X_k|² / (N · Σw²): soma dos P_k ≈ valor quadrático médio (Pa²).
    const float norm = 2.0f / ((float)n * (float)n * e->window_ms);
    const float df = e->cfg.sample_rate_hz / (float)n;
    double ez = 0.0, ea = 0.0, ec = 0.0, sfp = 0.0;
    e->power[0] = 0.0f;
    e->power[half] = 0.0f;
    for (uint32_t b = 1; b < half; ++b) {
        const float p = e->power[b] * norm;
        e->power[b] = p;
        ez += p;
        ea += (double)p * e->weight_a[b];
        ec += (double)p * e->weight_c[b];
        sfp += (double)p * (double)(df * (float)b);
        e->welch[b] += p;
    }
    e->welch_count++;
    if (ez > 0.0) {
        e->ratio_a = (float)(ea / ez);
        e->ratio_c = (float)(ec / ez);
        e->ratio_valid = true;
    }
    e->rep_fft_ez += ez;
    e->rep_fft_ea += ea;
    e->rep_fft_ec += ec;
    e->rep_m2 += m2;
    e->rep_m4 += m4;
    e->rep_kurt_n += n;

    if (event_frame) {
        e->ev_fft_ez += ez;
        e->ev_fft_ea += ea;
        e->ev_sum_fp += sfp;
        e->ev_sum_p += ez;
        // Planura do quadro (média geométrica / média aritmética), de 50 Hz a 90 % de Nyquist.
        uint32_t lo = (uint32_t)ceilf(50.0f / df);
        uint32_t hi = (uint32_t)(0.9f * (float)half);
        if (lo < 1u) lo = 1u;
        if (hi >= half) hi = half - 1u;
        if (hi > lo && ez > 0.0) {
            double log_sum = 0.0, lin_sum = 0.0;
            for (uint32_t b = lo; b <= hi; ++b) {
                log_sum += log((double)e->power[b] + 1e-30);
                lin_sum += e->power[b];
            }
            const double count = (double)(hi - lo + 1u);
            const double flat = lin_sum > 0.0 ? exp(log_sum / count) / (lin_sum / count) : 0.0;
            e->ev_flat_sum += flat * ez;
            e->ev_flat_w += ez;
        }
    }
}

static void frame_complete(AcousticEngine *e) {
    const AcousticFftPolicy policy = (AcousticFftPolicy)e->policy;
    const int32_t *done = e->frame[e->frame_cur];
    e->frame_cur ^= 1u;
    e->frame_fill = 0;
    e->rep_frames_done++;
    e->frames_since_fft++;
    const uint32_t frames_per_s = (uint32_t)(e->cfg.sample_rate_hz / (float)e->cfg.fft_size);
    bool want = false;
    switch (e->cfg.mode) {
        case ACOUSTIC_MODE_LEVEL:
        case ACOUSTIC_MODE_MACHINE:
            want = true;
            break;
        case ACOUSTIC_MODE_EVENTS:
            // Só o necessário: quadros durante eventos e um por segundo para a razão A/Z.
            want = e->ev_active || e->frames_since_fft >= (frames_per_s ? frames_per_s : 1u);
            break;
        default:
            break;
    }
    const bool run = want && (policy == ACOUSTIC_FFT_NORMAL || (policy == ACOUSTIC_FFT_LOADED && e->ev_active));
    e->prev_pending = !run;
    if (!run) return;
    analyze_frame(e, done, e->ev_active);
    e->frames_since_fft = 0;
    e->rep_frames_fft++;
}

/* --------------------------------------------------------------- eventos */

static void end_event(AcousticEngine *e, bool capped) {
    const float t_sub = e->t_sub;
    uint32_t above = e->ev_last_above ? e->ev_last_above : 1u;
    float duration_ms = (float)above * t_sub * 1000.0f;
    if (duration_ms > 65535.0f) duration_ms = 65535.0f;
    const double ra = e->ev_fft_ez > 0.0 ? e->ev_fft_ea / e->ev_fft_ez : (e->ratio_valid ? e->ratio_a : 1.0);
    const float centroid = e->ev_sum_p > 0.0 ? (float)(e->ev_sum_fp / e->ev_sum_p) : NAN;
    const float flatness = e->ev_flat_w > 0.0 ? (float)(e->ev_flat_sum / e->ev_flat_w) : NAN;

    uint8_t cls;
    if (duration_ms < (float)EVENT_SHORT_MS) {
        cls = (centroid == centroid && centroid >= EVENT_CRACK_HZ) ? ACOUSTIC_EVENT_CRACK : ACOUSTIC_EVENT_IMPACT;
    } else {
        cls = (flatness == flatness && flatness < EVENT_TONAL_FLATNESS) ? ACOUSTIC_EVENT_TONAL
                                                                         : ACOUSTIC_EVENT_TRANSIENT;
    }

    e->ev_last_class = cls;
    e->ev_last_duration_ms = (uint16_t)lroundf(duration_ms);
    e->ev_last_peak_db = db_pressure((double)e->ev_peak * (double)e->pa_per_count);
    // SEL: energia acima do fundo durante o evento, referida a 1 s.
    e->ev_last_sel_db = e->ev_excess > 0.0 ? db_power(e->ev_excess * ra) : NAN;
    e->ev_last_centroid = centroid;
    e->ev_last_end_samples = e->elapsed_samples;
    e->ev_any = true;
    e->ev_active = false;
    if (capped) e->lta = e->sta;   // ruído novo e contínuo: passa a ser o fundo
}

static void events_subframe(AcousticEngine *e, double ez, int32_t sub_peak) {
    if (!e->sl_valid) {
        e->sta = e->lta = (float)ez;
        e->sl_valid = true;
        e->warmup_sub = EVENT_WARMUP_S * ACOUSTIC_SUBFRAMES_PER_S;
        return;
    }
    e->sta += e->alpha_sta * ((float)ez - e->sta);
    if (!e->ev_active) e->lta += e->alpha_lta * ((float)ez - e->lta);
    if (e->warmup_sub > 0) {
        e->warmup_sub--;
        return;
    }
    const double floor = e->lta > 1e-14f ? (double)e->lta : 1e-14;
    const double ratio = (double)e->sta / floor;

    if (!e->ev_active && ratio > (double)e->cfg.threshold) {
        e->ev_active = true;
        e->ev_sub_count = 0;
        e->ev_last_above = 0;
        e->ev_excess = 0.0;
        e->ev_peak = 0;
        e->ev_fft_ez = e->ev_fft_ea = e->ev_sum_fp = e->ev_sum_p = 0.0;
        e->ev_flat_sum = e->ev_flat_w = 0.0;
        if (e->ev_count < UINT16_MAX) e->ev_count++;
        if (e->ev_interval < UINT8_MAX) e->ev_interval++;
        // O começo do evento pode estar no quadro que acabou de fechar sem FFT.
        if (e->prev_pending && e->policy != ACOUSTIC_FFT_NONE) {
            analyze_frame(e, e->frame[e->frame_cur ^ 1u], true);
            e->prev_pending = false;
            e->frames_since_fft = 0;
            e->rep_frames_fft++;
        }
    }
    if (!e->ev_active) return;

    e->ev_sub_count++;
    if (ez > 2.0 * floor) e->ev_last_above = e->ev_sub_count;
    if (ez > floor) e->ev_excess += (ez - floor) * (double)e->t_sub;
    if (sub_peak > e->ev_peak) e->ev_peak = sub_peak;
    const bool capped = e->ev_sub_count >= EVENT_MAX_S * ACOUSTIC_SUBFRAMES_PER_S;
    if (ratio < EVENT_END_RATIO || capped) end_event(e, capped);
}

/* -------------------------------------------------------------- dose e Ln */

static void add_dose(AcousticEngine *e, float las_db, double seconds) {
    if (!(las_db >= DOSE_THRESHOLD_DB)) return;
    const double x = (double)las_db - DOSE_CRITERION_DB;
    e->dose_nr15 += seconds / DOSE_DAY_S * pow(2.0, x / 5.0);
    e->dose_nho01 += seconds / DOSE_DAY_S * pow(2.0, x / 3.0);
}

static float ln_percentile(const uint32_t *hist, uint32_t total, double exceeded) {
    if (total == 0) return NAN;
    uint32_t target = (uint32_t)ceil(exceeded * (double)total);
    if (target == 0) target = 1;
    uint32_t acc = 0;
    for (int b = ACOUSTIC_LN_BINS - 1; b >= 0; --b) {
        acc += hist[b];
        if (acc >= target) return ((float)b + 0.5f) * ACOUSTIC_LN_BIN_DB;
    }
    return 0.0f;
}

/* -------------------------------------------------------------- subquadro */

static void finish_report(AcousticEngine *e);

static void subframe_complete(AcousticEngine *e) {
    const uint32_t n = e->sub_n;

    // Microfone: palavras todas em zero (SD aberto) ou todas em um (SD preso alto).
    if (e->sub_or == 0) {
        e->silent_sub++;
        e->high_sub = 0;
    } else if (e->sub_and == -1) {
        e->high_sub++;
        e->silent_sub = 0;
    } else {
        e->silent_sub = 0;
        e->high_sub = 0;
    }
    if (e->silent_sub >= MIC_FAULT_SUB) e->mic_state = ACOUSTIC_MIC_SILENT;
    else if (e->high_sub >= MIC_FAULT_SUB) e->mic_state = ACOUSTIC_MIC_STUCK_HIGH;
    else if (e->silent_sub == 0 && e->high_sub == 0) e->mic_state = ACOUSTIC_MIC_OK;

    const double mean = (double)e->sub_sum / (double)n;
    if (!e->dc_valid) {
        e->dc = mean;
        e->dc_valid = true;
    } else {
        e->dc += DC_ALPHA * (mean - e->dc);
    }
    // Σ(s - dc)² = Σs² - 2·dc·Σs + n·dc²
    double ss = (double)e->sub_sumsq - 2.0 * e->dc * (double)e->sub_sum + (double)n * e->dc * e->dc;
    if (ss < 0.0) ss = 0.0;
    const double k2 = (double)e->pa_per_count * (double)e->pa_per_count;
    const double ez = ss / (double)n * k2;      // Pa², valor quadrático médio do subquadro
    e->rep_energy_z += ss * k2;
    e->rep_n += n;
    if (e->sub_peak > e->rep_peak) e->rep_peak = e->sub_peak;
    e->elapsed_samples += (double)n;

    const double ea = ez * (double)(e->ratio_valid ? e->ratio_a : 1.0f);
    if (!e->tw_valid) {
        e->e_fast = e->e_slow = (float)ea;
        e->tw_valid = true;
    } else {
        e->e_fast += e->alpha_fast * ((float)ea - e->e_fast);
        e->e_slow += e->alpha_slow * ((float)ea - e->e_slow);
    }
    const float laf = db_power(e->e_fast);
    const float las = db_power(e->e_slow);
    if (laf > e->laf_max) e->laf_max = laf;
    if (laf < e->laf_min) e->laf_min = laf;
    if (las > e->las_max) e->las_max = las;

    if (e->cfg.mode == ACOUSTIC_MODE_LEVEL) {
        int bin = (int)(laf / ACOUSTIC_LN_BIN_DB);
        if (bin < 0) bin = 0;
        if (bin >= ACOUSTIC_LN_BINS) bin = ACOUSTIC_LN_BINS - 1;
        e->ln_hist[bin]++;
        e->ln_count++;
        add_dose(e, las, (double)e->t_sub);
    } else if (e->cfg.mode == ACOUSTIC_MODE_EVENTS) {
        events_subframe(e, ez, e->sub_peak);
    }

    e->sub_sum = e->sub_sumsq = 0;
    e->sub_peak = 0;
    e->sub_or = 0;
    e->sub_and = -1;
    e->sub_n = 0;

    if (e->rep_n >= e->report_len) finish_report(e);
}

/* ---------------------------------------------------------------- entrada */

void acoustic_engine_process(AcousticEngine *e, const int32_t *words, size_t count, AcousticFftPolicy policy) {
    if (!e || !words || !e->configured || e->cfg.mode == ACOUSTIC_MODE_IDLE) return;
    e->policy = (uint8_t)policy;
    const uint32_t frame_len = e->cfg.fft_size;
    size_t i = 0;
    while (i < count) {
        uint32_t chunk = (uint32_t)(count - i);
        const uint32_t to_sub = e->subframe_len - e->sub_n;
        const uint32_t to_frame = frame_len - e->frame_fill;
        if (chunk > to_sub) chunk = to_sub;
        if (chunk > to_frame) chunk = to_frame;

        // Laço por amostra: só inteiros. A soma dos quadrados de 24 bits cabe
        // com folga em 64 bits num subquadro (até 480 amostras).
        int64_t sum = e->sub_sum;
        int64_t sumsq = e->sub_sumsq;
        int32_t peak = e->sub_peak;
        int32_t acc_or = e->sub_or;
        int32_t acc_and = e->sub_and;
        uint32_t clip = 0;
        int32_t *dst = &e->frame[e->frame_cur][e->frame_fill];
        const int32_t *src = &words[i];
        for (uint32_t j = 0; j < chunk; ++j) {
            const int32_t s = src[j] >> 8;
            sum += s;
            sumsq += (int64_t)s * (int64_t)s;
            const int32_t a = s < 0 ? -s : s;
            if (a > peak) peak = a;
            if (a >= ACOUSTIC_CLIP_COUNTS) clip++;
            acc_or |= s;
            acc_and &= s;
            dst[j] = s;
        }
        e->sub_sum = sum;
        e->sub_sumsq = sumsq;
        e->sub_peak = peak;
        e->sub_or = acc_or;
        e->sub_and = acc_and;
        e->sub_n += chunk;
        e->frame_fill += chunk;
        e->rep_overload += clip;
        i += chunk;

        // O quadro fecha antes do subquadro: assim a FFT de um quadro que
        // termina junto com o relatório entra nesse relatório.
        if (e->frame_fill >= frame_len) frame_complete(e);
        if (e->sub_n >= e->subframe_len) subframe_complete(e);
    }
}

void acoustic_engine_note_gap(AcousticEngine *e, uint32_t lost_samples) {
    if (!e || !e->configured || lost_samples == 0) return;
    e->rep_lost += lost_samples;
    e->elapsed_samples += (double)lost_samples;
    // Sem as amostras, a dose segue o último nível lento conhecido.
    if (e->cfg.mode == ACOUSTIC_MODE_LEVEL && e->tw_valid) {
        add_dose(e, db_power(e->e_slow), (double)lost_samples / (double)e->cfg.sample_rate_hz);
    }
}

bool acoustic_engine_take_report(AcousticEngine *e, AcousticReport *out) {
    if (!e || !e->report_ready) return false;
    if (out) *out = e->report;
    e->report_ready = false;
    return true;
}

/* -------------------------------------------------------- espectro médio */

static float spectrum_peak(const AcousticEngine *e, const float *p, uint32_t half, float df, uint32_t *peak_bin) {
    uint32_t k_lo = (uint32_t)ceilf(20.0f / df);
    if (k_lo < 1u) k_lo = 1u;
    uint32_t best = 0;
    float best_p = 0.0f;
    for (uint32_t b = k_lo; b < half; ++b) {
        if (p[b] > best_p) {
            best_p = p[b];
            best = b;
        }
    }
    (void)e;
    if (peak_bin) *peak_bin = best;
    if (best == 0 || best_p <= 0.0f) return NAN;
    float delta = 0.0f;
    if (best > 1u && best + 1u < half && p[best - 1u] > 0.0f && p[best + 1u] > 0.0f) {
        const double a = log((double)p[best - 1u]);
        const double b = log((double)p[best]);
        const double c = log((double)p[best + 1u]);
        const double den = a - 2.0 * b + c;
        if (den < 0.0) delta = (float)(0.5 * (a - c) / den);
        if (delta > 0.5f) delta = 0.5f;
        if (delta < -0.5f) delta = -0.5f;
    }
    return ((float)best + delta) * df;
}

// Mediana de até 64 valores (ordenação por inserção numa cópia).
static float median_of(const float *values, uint32_t count) {
    float tmp[64];
    if (count == 0) return 0.0f;
    if (count > 64u) count = 64u;
    for (uint32_t i = 0; i < count; ++i) {
        const float v = values[i];
        uint32_t j = i;
        while (j > 0 && tmp[j - 1u] > v) {
            tmp[j] = tmp[j - 1u];
            j--;
        }
        tmp[j] = v;
    }
    return (count & 1u) ? tmp[count / 2u] : 0.5f * (tmp[count / 2u - 1u] + tmp[count / 2u]);
}

// Proeminência do componente dominante: potência do tom (3 faixas) contra a
// densidade mediana do ruído no terço de oitava em volta, sem as faixas do tom.
// A mediana ignora os lóbulos de outros harmônicos que caiam na referência.
static float tonal_prominence(const float *p, uint32_t half, uint32_t peak, float df) {
    if (peak < 1u || peak + 1u >= half) return NAN;
    const double tone = (double)p[peak - 1u] + (double)p[peak] + (double)p[peak + 1u];
    const float f = (float)peak * df;
    int32_t lo = (int32_t)floorf(f * 0.8909f / df);   // 2^(-1/6)
    int32_t hi = (int32_t)ceilf(f * 1.1225f / df);    // 2^(+1/6)
    if (hi - lo < 20) {
        lo = (int32_t)peak - 10;
        hi = (int32_t)peak + 10;
    }
    if (lo < (int32_t)peak - 32) lo = (int32_t)peak - 32;
    if (hi > (int32_t)peak + 32) hi = (int32_t)peak + 32;
    if (lo < 1) lo = 1;
    if (hi > (int32_t)half - 1) hi = (int32_t)half - 1;
    float ref[64];
    uint32_t count = 0;
    for (int32_t b = lo; b <= hi && count < 64u; ++b) {
        if (b >= (int32_t)peak - 3 && b <= (int32_t)peak + 3) continue;
        ref[count++] = p[b];
    }
    if (count < 4u || tone <= 0.0) return NAN;
    const double density = median_of(ref, count);
    if (density <= 0.0) return 60.0f;
    float db = (float)(10.0 * log10(tone / (3.0 * density)));
    return db > 60.0f ? 60.0f : db;
}

// Piso entre harmônicos: média dos vales a meio caminho dos vizinhos.
static float valley_floor(const float *p, uint32_t half, float center_k, float k0) {
    double sum = 0.0;
    uint32_t count = 0;
    const float sides[2] = {center_k - 0.5f * k0, center_k + 0.5f * k0};
    for (unsigned s = 0; s < 2; ++s) {
        const int32_t c = (int32_t)lroundf(sides[s]);
        for (int32_t b = c - 1; b <= c + 1; ++b) {
            if (b < 1 || b >= (int32_t)half) continue;
            sum += p[b];
            count++;
        }
    }
    return count ? (float)(sum / (double)count) : 0.0f;
}

static float local_max(const float *p, uint32_t half, int32_t center, uint32_t *at) {
    float best = 0.0f;
    uint32_t best_at = 0;
    for (int32_t b = center - 1; b <= center + 1; ++b) {
        if (b < 1 || b >= (int32_t)half) continue;
        if (p[b] > best) {
            best = p[b];
            best_at = (uint32_t)b;
        }
    }
    if (at) *at = best_at;
    return best;
}

// Fundamental de uma série harmônica pelo produto harmônico do espectro (HPS),
// em escala logarítmica e grade de 1/4 de faixa; depois confere quantos
// harmônicos se destacam 6 dB acima dos vales vizinhos e refina f0 pelo
// harmônico mais forte. É o "tacômetro acústico" do modo MACHINE. A
// fundamental precisa estar a 4 faixas ou mais (f0 >= 4·fs/N) para que os
// lóbulos dos harmônicos não se sobreponham.
static float harmonic_f0(const float *p, uint32_t half, float df, uint8_t *harmonics) {
    *harmonics = 0;
    const float k_lo = fmaxf(4.0f, 20.0f / df);
    const float k_hi = (float)(half - 2u) / (float)HPS_HARMONICS;
    if (k_hi <= k_lo) return 0.0f;
    float best_score = -INFINITY;
    float best_k = 0.0f;
    for (float kf = k_lo; kf <= k_hi; kf += 0.25f) {
        float score = 0.0f;
        for (unsigned h = 1; h <= HPS_HARMONICS; ++h) {
            const float m = local_max(p, half, (int32_t)lroundf(kf * (float)h), NULL);
            score += logf(m + 1e-30f);
        }
        if (score > best_score) {
            best_score = score;
            best_k = kf;
        }
    }
    if (best_k <= 0.0f) return 0.0f;

    uint8_t present = 0;
    bool low_present = false;
    float strongest = 0.0f;
    uint32_t strongest_bin = 0;
    unsigned strongest_h = 0;
    for (unsigned h = 1; h <= HARMONIC_MAX; ++h) {
        const int32_t center = (int32_t)lroundf(best_k * (float)h);
        if (center + 1 >= (int32_t)half) break;
        uint32_t at = 0;
        const float m = local_max(p, half, center, &at);
        const float fl = valley_floor(p, half, best_k * (float)h, best_k);
        if (m > 0.0f && m > 4.0f * fl) {
            present++;
            if (h <= 2) low_present = true;
            if (m > strongest) {
                strongest = m;
                strongest_bin = at;
                strongest_h = h;
            }
        }
    }
    if (present < HARMONIC_MIN_VALID || !low_present || strongest_h == 0) return 0.0f;
    *harmonics = present;
    float delta = 0.0f;
    if (strongest_bin > 1u && strongest_bin + 1u < half && p[strongest_bin - 1u] > 0.0f &&
        p[strongest_bin + 1u] > 0.0f) {
        const double a = log((double)p[strongest_bin - 1u]);
        const double b = log((double)p[strongest_bin]);
        const double c = log((double)p[strongest_bin + 1u]);
        const double den = a - 2.0 * b + c;
        if (den < 0.0) delta = (float)(0.5 * (a - c) / den);
        if (delta > 0.5f) delta = 0.5f;
        if (delta < -0.5f) delta = -0.5f;
    }
    return ((float)strongest_bin + delta) * df / (float)strongest_h;
}

/* --------------------------------------------------------------- relatório */

static void finish_report(AcousticEngine *e) {
    AcousticReport *r = &e->report;
    const float fs = e->cfg.sample_rate_hz;
    const uint32_t n = e->cfg.fft_size;
    const uint32_t half = n / 2u;
    const float df = fs / (float)n;
    const float k = e->pa_per_count;

    memset(r, 0, sizeof(*r));
    r->mode = e->cfg.mode;
    r->fft_size = (uint16_t)n;
    r->sample_rate_hz = fs;
    r->samples = e->rep_n;
    r->coverage_pct = (uint8_t)(e->rep_frames_done
                                    ? (e->rep_frames_fft >= e->rep_frames_done
                                           ? 100u
                                           : (100u * e->rep_frames_fft) / e->rep_frames_done)
                                    : 0u);
    r->laeq = r->lceq = r->lzeq = r->lzpeak = r->laf = r->lafmax = r->lafmin = NAN;
    r->crest = r->kurtosis = r->peak_hz = r->centroid_hz = r->flatness = NAN;
    r->leq_total = r->l10 = r->l50 = r->l90 = NAN;
    r->dose_nr15_pct = r->dose_nho01_pct = r->las_max = NAN;
    r->tonal_db = r->anomaly_z = NAN;
    r->event_peak_db = r->event_sel_db = r->event_centroid_hz = NAN;
    r->sta_lta_db = r->background_db = NAN;
    r->event_age_s = ACOUSTIC_AGE_NONE;
    for (unsigned b = 0; b < ACOUSTIC_BANDS; ++b) r->octave_db[b] = NAN;

    const double ms_z = e->rep_n ? e->rep_energy_z / (double)e->rep_n : 0.0;
    r->lzeq = db_power(ms_z);
    double ra = NAN, rc = NAN;
    if (e->rep_fft_ez > 0.0) {
        ra = e->rep_fft_ea / e->rep_fft_ez;
        rc = e->rep_fft_ec / e->rep_fft_ez;
        r->flags |= ACOUSTIC_FLAG_SPECTRUM_VALID;
    } else if (e->ratio_valid) {
        ra = e->ratio_a;
        rc = e->ratio_c;
    }
    if (ra == ra) r->laeq = db_power(ms_z * ra);
    if (rc == rc) r->lceq = db_power(ms_z * rc);
    r->lzpeak = db_pressure((double)e->rep_peak * (double)k);
    if (e->tw_valid) r->laf = db_power(e->e_fast);
    if (e->laf_max > -INFINITY) r->lafmax = e->laf_max;
    if (e->laf_min < INFINITY) r->lafmin = e->laf_min;
    if (ms_z > 0.0) r->crest = (float)((double)e->rep_peak * (double)k / sqrt(ms_z));
    if (e->rep_kurt_n > 0 && e->rep_m2 > 0.0) {
        const double m2 = e->rep_m2 / (double)e->rep_kurt_n;
        const double m4 = e->rep_m4 / (double)e->rep_kurt_n;
        r->kurtosis = (float)(m4 / (m2 * m2) - 3.0);
    }
    r->overload_samples = e->rep_overload;
    if (e->rep_overload > 0) r->flags |= ACOUSTIC_FLAG_OVERLOAD;
    r->lost_samples = e->rep_lost;
    r->mic_state = e->mic_state;
    if (e->mic_state != ACOUSTIC_MIC_OK) r->flags |= ACOUSTIC_FLAG_MIC_FAULT;

    // Espectro médio do intervalo (Welch, sem sobreposição).
    float *avg = e->welch;
    uint32_t peak_bin = 0;
    if (e->welch_count > 0) {
        const float inv = 1.0f / (float)e->welch_count;
        double sum_p = 0.0, sum_fp = 0.0;
        for (uint32_t b = 1; b < half; ++b) {
            avg[b] *= inv;
            sum_p += avg[b];
            sum_fp += (double)avg[b] * (double)(df * (float)b);
        }
        r->peak_hz = spectrum_peak(e, avg, half, df, &peak_bin);
        if (sum_p > 0.0) r->centroid_hz = (float)(sum_fp / sum_p);
        uint32_t lo = (uint32_t)ceilf(50.0f / df);
        uint32_t hi = (uint32_t)(0.9f * (float)half);
        if (lo < 1u) lo = 1u;
        if (hi >= half) hi = half - 1u;
        if (hi > lo) {
            double log_sum = 0.0, lin_sum = 0.0;
            for (uint32_t b = lo; b <= hi; ++b) {
                log_sum += log((double)avg[b] + 1e-30);
                lin_sum += avg[b];
            }
            const double count = (double)(hi - lo + 1u);
            if (lin_sum > 0.0) r->flatness = (float)(exp(log_sum / count) / (lin_sum / count));
        }
        // Bandas de oitava: soma da potência das faixas dentro de [fc/√2, fc·√2).
        for (unsigned band = 0; band < ACOUSTIC_BANDS; ++band) {
            const double fc = 1000.0 * pow(2.0, (double)band - 5.0);
            const double f_lo = fc / M_SQRT2;
            const double f_hi = fc * M_SQRT2;
            if (f_hi > (double)fs / 2.0) continue;
            uint32_t b_lo = (uint32_t)ceil(f_lo / (double)df);
            uint32_t b_hi = (uint32_t)ceil(f_hi / (double)df);
            if (b_lo < 1u) b_lo = 1u;
            double band_p = 0.0;
            uint32_t bins = 0;
            for (uint32_t b = b_lo; b < b_hi && b < half; ++b) {
                band_p += avg[b];
                bins++;
            }
            if (bins > 0) r->octave_db[band] = db_power(band_p);
        }
        // Espectro reduzido para o Node: potência somada em grupos, enviada como Pa eficaz.
        const uint32_t out = half < ACOUSTIC_SPECTRUM_OUT_BINS ? half : ACOUSTIC_SPECTRUM_OUT_BINS;
        const uint32_t group = half / out;
        for (uint32_t g = 0; g < out; ++g) {
            double s = 0.0;
            for (uint32_t b = g * group; b < (g + 1u) * group; ++b) s += (b == 0) ? 0.0 : avg[b];
            r->spectrum_pa[g] = (float)sqrt(s);
        }
        r->spectrum_bins = (uint16_t)out;
    }

    // Valor efetivo da ponderação A no intervalo, para os acumuladores desde o reinício.
    const double ra_total = ra == ra ? ra : (e->ratio_valid ? (double)e->ratio_a : 1.0);
    const double seconds = (double)e->rep_n / (double)fs;

    switch (e->cfg.mode) {
        case ACOUSTIC_MODE_LEVEL: {
            e->energy_a_total += ms_z * ra_total * seconds;
            const double elapsed = e->elapsed_samples / (double)fs;
            if (elapsed > 0.0) r->leq_total = db_power(e->energy_a_total / elapsed);
            r->l10 = ln_percentile(e->ln_hist, e->ln_count, 0.10);
            r->l50 = ln_percentile(e->ln_hist, e->ln_count, 0.50);
            r->l90 = ln_percentile(e->ln_hist, e->ln_count, 0.90);
            r->dose_nr15_pct = (float)(e->dose_nr15 * 100.0);
            r->dose_nho01_pct = (float)(e->dose_nho01 * 100.0);
            if (e->las_max > -INFINITY) r->las_max = e->las_max;
            r->elapsed_s = (uint32_t)elapsed;
            if (e->dose_nr15 >= 1.0 || e->dose_nho01 >= 1.0 || (e->las_max > CEILING_DBA)) {
                r->flags |= ACOUSTIC_FLAG_DOSE_LIMIT;
            }
            break;
        }
        case ACOUSTIC_MODE_MACHINE: {
            if (e->welch_count > 0 && peak_bin > 0) {
                r->tonal_db = tonal_prominence(avg, half, peak_bin, df);
                uint8_t harmonics = 0;
                r->f0_hz = harmonic_f0(avg, half, df, &harmonics);
                r->harmonics = harmonics;
            }
            // Assinatura de referência por banda de oitava.
            if (e->learn_left_s > 0) {
                for (unsigned band = 0; band < ACOUSTIC_BANDS; ++band) {
                    const float level = r->octave_db[band];
                    if (level != level) continue;
                    const uint32_t count = ++e->base_n[band];
                    const double delta = (double)level - e->base_mean[band];
                    e->base_mean[band] += delta / (double)count;
                    e->base_m2[band] += delta * ((double)level - e->base_mean[band]);
                }
                e->learn_left_s--;
                if (e->learn_left_s == 0) {
                    unsigned usable = 0;
                    for (unsigned band = 0; band < ACOUSTIC_BANDS; ++band) usable += e->base_n[band] >= 3u;
                    e->baseline_ready = usable >= 3u;
                }
            } else if (e->baseline_ready) {
                double sum_z2 = 0.0;
                unsigned count = 0;
                for (unsigned band = 0; band < ACOUSTIC_BANDS; ++band) {
                    const float level = r->octave_db[band];
                    if (level != level || e->base_n[band] < 3u) continue;
                    double sd = sqrt(e->base_m2[band] / (double)(e->base_n[band] - 1u));
                    if (sd < BASELINE_MIN_SD_DB) sd = BASELINE_MIN_SD_DB;
                    const double z = ((double)level - e->base_mean[band]) / sd;
                    sum_z2 += z * z;
                    count++;
                }
                if (count > 0) {
                    r->anomaly_z = (float)sqrt(sum_z2 / (double)count);
                    if (r->anomaly_z > e->cfg.threshold) r->flags |= ACOUSTIC_FLAG_ANOMALY;
                }
            }
            if (e->learn_total_s > 0 && e->learn_left_s > 0) {
                r->flags |= ACOUSTIC_FLAG_BASELINE_LEARNING;
                r->baseline_pct = (uint8_t)(100u * (uint32_t)(e->learn_total_s - e->learn_left_s) / e->learn_total_s);
            } else if (e->baseline_ready) {
                r->flags |= ACOUSTIC_FLAG_BASELINE_READY;
                r->baseline_pct = 100;
            }
            break;
        }
        case ACOUSTIC_MODE_EVENTS: {
            r->event_count = e->ev_count;
            r->events_interval = e->ev_interval;
            if (e->ev_any) {
                r->event_class = e->ev_last_class;
                r->event_peak_db = e->ev_last_peak_db;
                r->event_sel_db = e->ev_last_sel_db;
                r->event_centroid_hz = e->ev_last_centroid;
                r->event_duration_ms = e->ev_last_duration_ms;
                const double age = (e->elapsed_samples - e->ev_last_end_samples) / (double)fs;
                r->event_age_s = (uint16_t)(age < 65534.0 ? (age > 0.0 ? age : 0.0) : 65534.0);
            }
            if (e->sl_valid && e->lta > 0.0f) {
                r->sta_lta_db = (float)(10.0 * log10((double)e->sta / (double)e->lta));
                r->background_db = db_power((double)e->lta * ra_total);
            }
            if (e->ev_active) r->flags |= ACOUSTIC_FLAG_EVENT_ACTIVE;
            break;
        }
        default:
            break;
    }

    e->report_ready = true;
    clear_interval(e);
}
