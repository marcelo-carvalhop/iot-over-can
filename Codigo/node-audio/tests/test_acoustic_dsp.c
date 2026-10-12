/*
 * Testes nativos do motor acústico (acoustic_dsp.c e audio_fft.c), executados
 * no computador de desenvolvimento e na integração contínua.
 *
 * Os sinais são sintetizados em pascal e convertidos em palavras I2S de 24
 * bits com a sensibilidade nominal do INMP441, exatamente como chegariam pela
 * PIO. Assim os níveis esperados são os da física (94 dB SPL = 1,0024 Pa) e não
 * dependem de nenhum detalhe interno do motor.
 *
 *   test_acoustic_dsp           executa as verificações
 *   test_acoustic_dsp --bench   mede o custo por segundo de áudio no computador
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "acoustic_dsp.h"
#include "audio_fft.h"
#include "edge_protocol_definitions.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(value, expected, tol)                                                          \
    do {                                                                                          \
        const double v_ = (double)(value);                                                        \
        const double e_ = (double)(expected);                                                     \
        if (!(fabs(v_ - e_) <= (double)(tol))) {                                                  \
            fprintf(stderr, "FALHA %s:%d: %s = %.4f, esperado %.4f ± %.4f\n", __FILE__, __LINE__, \
                    #value, v_, e_, (double)(tol));                                               \
            g_failures++;                                                                         \
        }                                                                                         \
    } while (0)

static AcousticEngine g_engine;

/* --------------------------------------------------------------- sinais */

static uint32_t g_rng = 0x12345678u;

static double uniform(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (double)g_rng / 4294967296.0;
}

// Ruído aproximadamente gaussiano de variância 1 (soma de 12 uniformes).
static double gauss(void) {
    double s = 0.0;
    for (int i = 0; i < 12; ++i) s += uniform();
    return s - 6.0;
}

static double pa_rms(double db) { return 20e-6 * pow(10.0, db / 20.0); }

static int32_t to_word(double pa) {
    double counts = pa / (double)ACOUSTIC_PA_PER_FULL_SCALE * 8388608.0;
    if (counts > 8388607.0) counts = 8388607.0;
    if (counts < -8388608.0) counts = -8388608.0;
    const int32_t s = (int32_t)lround(counts);
    return (int32_t)((uint32_t)s << 8);   // 24 bits alinhados à esquerda, como a PIO entrega
}

typedef double (*signal_fn)(double t, void *ctx);

typedef struct {
    double freq[12];
    double amp[12];      // Pa eficaz de cada componente
    unsigned count;
    double noise_rms;    // Pa eficaz de ruído branco
} Mix;

static double mix_signal(double t, void *ctx) {
    const Mix *m = (const Mix *)ctx;
    double v = 0.0;
    for (unsigned i = 0; i < m->count; ++i) v += m->amp[i] * sqrt(2.0) * sin(2.0 * M_PI * m->freq[i] * t + 0.3 * i);
    if (m->noise_rms > 0.0) v += m->noise_rms * gauss();
    return v;
}

static AcousticReport g_reports[64];
static unsigned g_report_count = 0;
static double g_time = 0.0;

// Alimenta `seconds` de sinal em blocos de 256 palavras (o tamanho do bloco de DMA).
static void feed(signal_fn fn, void *ctx, double seconds, AcousticFftPolicy policy) {
    const double fs = g_engine.cfg.sample_rate_hz;
    const size_t total = (size_t)llround(seconds * fs);
    int32_t block[256];
    size_t done = 0;
    while (done < total) {
        size_t n = total - done;
        if (n > 256) n = 256;
        for (size_t i = 0; i < n; ++i) {
            block[i] = to_word(fn(g_time, ctx));
            g_time += 1.0 / fs;
        }
        acoustic_engine_process(&g_engine, block, n, policy);
        AcousticReport r;
        if (acoustic_engine_take_report(&g_engine, &r)) {
            g_reports[g_report_count % 64] = r;
            g_report_count++;
        }
        done += n;
    }
}

static const AcousticReport *last_report(void) {
    return g_report_count ? &g_reports[(g_report_count - 1) % 64] : NULL;
}

static void setup(uint8_t mode, float fs, uint16_t n, uint8_t window, float threshold) {
    acoustic_engine_init(&g_engine);
    AcousticConfig cfg = {mode, fs, n, window, threshold, 1.0f};
    CHECK(acoustic_engine_configure(&g_engine, &cfg));
    g_report_count = 0;
    g_time = 0.0;
}

static void mix_tone(Mix *m, double f, double db) {
    memset(m, 0, sizeof(*m));
    m->freq[0] = f;
    m->amp[0] = pa_rms(db);
    m->count = 1;
}

/* ---------------------------------------------------------------- testes */

static void test_weighting_curves(void) {
    // IEC 61672-1, tabela 3 (valores de projeto), nas frequências exatas de
    // base 10 (fm = 1000 · 10^(n/10) Hz), não nas nominais.
    const struct { float f, a, c; } table[] = {
        {31.623f, -39.4f, -3.0f}, {63.096f, -26.2f, -0.8f}, {125.89f, -16.1f, -0.2f}, {251.19f, -8.6f, 0.0f},
        {501.19f, -3.2f, 0.0f},   {1000.0f, 0.0f, 0.0f},    {1995.3f, 1.2f, -0.2f},   {3981.1f, 1.0f, -0.8f},
        {7943.3f, -1.1f, -3.0f},  {15849.0f, -6.6f, -8.5f},
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        CHECK_NEAR(acoustic_weight_a_db(table[i].f), table[i].a, 0.1);
        CHECK_NEAR(acoustic_weight_c_db(table[i].f), table[i].c, 0.1);
    }
    CHECK_NEAR(acoustic_band_center_hz(0), 31.5, 1e-6);
    CHECK_NEAR(acoustic_band_center_hz(9), 16000.0, 1e-6);
}

static void test_fft_against_dft(void) {
    static AudioFft fft;
    static float x[AUDIO_FFT_MAX_N], power[AUDIO_FFT_MAX_N / 2 + 1];
    static double xd[AUDIO_FFT_MAX_N];
    CHECK(!audio_fft_init(&fft, 100));
    CHECK(!audio_fft_init(&fft, 128));
    CHECK(!audio_fft_init(&fft, 4096));
    for (uint16_t n = 256; n <= AUDIO_FFT_MAX_N; n = (uint16_t)(n * 2u)) {
        CHECK(audio_fft_init(&fft, n));
        for (uint16_t i = 0; i < n; ++i) {
            xd[i] = 0.7 * sin(2.0 * M_PI * 37.3 * i / n) + 0.3 * (uniform() - 0.5);
            x[i] = (float)xd[i];
        }
        audio_fft_power(&fft, x, power);
        double worst = 0.0, peak = 0.0;
        for (uint16_t k = 0; k <= n / 2; k += 7) {
            double re = 0.0, im = 0.0;
            for (uint16_t i = 0; i < n; ++i) {
                re += xd[i] * cos(2.0 * M_PI * k * i / n);
                im -= xd[i] * sin(2.0 * M_PI * k * i / n);
            }
            const double p = re * re + im * im;
            if (p > peak) peak = p;
            const double err = fabs(p - power[k]);
            if (err > worst) worst = err;
        }
        CHECK(worst <= 1e-5 * (peak > 1.0 ? peak : 1.0));
    }
}

static void test_calibration_1khz_94db(void) {
    Mix m;
    mix_tone(&m, 1000.0, 94.0);
    setup(ACOUSTIC_MODE_LEVEL, 16000.0f, 1024, 1, 8.0f);
    feed(mix_signal, &m, 4.0, ACOUSTIC_FFT_NORMAL);
    CHECK(g_report_count == 4);
    const AcousticReport *r = last_report();
    CHECK(r->samples == 16000);
    CHECK(r->coverage_pct == 100);
    CHECK(r->flags & ACOUSTIC_FLAG_SPECTRUM_VALID);
    CHECK_NEAR(r->lzeq, 94.0, 0.05);
    CHECK_NEAR(r->laeq, 94.0, 0.1);
    CHECK_NEAR(r->lceq, 94.0, 0.1);
    CHECK_NEAR(r->lzpeak, 97.01, 0.1);       // pico de senoide: +3,01 dB
    CHECK_NEAR(r->crest, sqrt(2.0), 0.02);
    CHECK_NEAR(r->kurtosis, -1.5, 0.05);     // excesso de curtose de uma senoide
    CHECK_NEAR(r->laf, 94.0, 0.1);
    CHECK_NEAR(r->lafmax, 94.0, 0.15);
    CHECK_NEAR(r->peak_hz, 1000.0, 1.0);
    CHECK(r->flatness < 0.05f);
    CHECK_NEAR(r->octave_db[5], 94.0, 0.3);  // banda de 1 kHz
    CHECK(r->octave_db[3] < 60.0f);          // 250 Hz: só vazamento da janela
    CHECK(isnan(r->octave_db[9]));           // 16 kHz acima de Nyquist (8 kHz)
    CHECK(r->spectrum_bins == 256);
    unsigned best = 0;
    for (unsigned g = 0; g < r->spectrum_bins; ++g) if (r->spectrum_pa[g] > r->spectrum_pa[best]) best = g;
    CHECK_NEAR(best * 8000.0 / 256.0, 1000.0, 32.0);
    CHECK(r->spectrum_pa[best] > 0.7f && r->spectrum_pa[best] < 1.05f);
    CHECK(!(r->flags & ACOUSTIC_FLAG_OVERLOAD));
    CHECK(r->mic_state == ACOUSTIC_MIC_OK);

    // Ganho de calibração: +1 dB de pressão desloca todos os níveis.
    AcousticConfig cfg = g_engine.cfg;
    cfg.gain = (float)pow(10.0, 1.0 / 20.0);
    CHECK(acoustic_engine_configure(&g_engine, &cfg));
    feed(mix_signal, &m, 2.0, ACOUSTIC_FFT_NORMAL);
    CHECK_NEAR(last_report()->lzeq, 95.0, 0.05);
}

static void test_frequency_weighting_in_levels(void) {
    Mix m;
    mix_tone(&m, 100.0, 80.0);
    setup(ACOUSTIC_MODE_LEVEL, 16000.0f, 2048, 1, 8.0f);
    feed(mix_signal, &m, 3.0, ACOUSTIC_FFT_NORMAL);
    const AcousticReport *r = last_report();
    CHECK_NEAR(r->lzeq, 80.0, 0.05);
    CHECK_NEAR(r->laeq, 80.0 + acoustic_weight_a_db(100.0f), 0.3);
    CHECK_NEAR(r->lceq, 80.0 + acoustic_weight_c_db(100.0f), 0.3);

    mix_tone(&m, 10000.0, 85.0);
    setup(ACOUSTIC_MODE_LEVEL, 48000.0f, 2048, 1, 8.0f);
    feed(mix_signal, &m, 3.0, ACOUSTIC_FFT_NORMAL);
    r = last_report();
    CHECK(r->samples == 48000);
    CHECK_NEAR(r->laeq, 85.0 - 2.5, 0.3);
    CHECK(!isnan(r->octave_db[9]));          // a 48 kHz as dez bandas existem
    CHECK_NEAR(r->octave_db[8], 85.0, 0.5);  // 10 kHz cai na banda de 8 kHz (5,6 a 11,3 kHz)
}

static void test_broadband_noise(void) {
    Mix m;
    memset(&m, 0, sizeof(m));
    m.noise_rms = pa_rms(70.0);
    setup(ACOUSTIC_MODE_LEVEL, 16000.0f, 1024, 1, 8.0f);
    feed(mix_signal, &m, 3.0, ACOUSTIC_FFT_NORMAL);
    const AcousticReport *r = last_report();
    CHECK_NEAR(r->lzeq, 70.0, 0.2);
    CHECK(r->flatness > 0.5f);
    CHECK(r->laeq < r->lzeq + 1.5f);
    CHECK_NEAR(r->kurtosis, 0.0, 0.3);       // gaussiano
    CHECK(r->centroid_hz > 3000.0f && r->centroid_hz < 5000.0f);  // ruído branco: ~fs/4
}

static void test_dose_and_statistics(void) {
    Mix m;
    mix_tone(&m, 1000.0, 90.0);
    setup(ACOUSTIC_MODE_LEVEL, 8000.0f, 1024, 1, 8.0f);
    feed(mix_signal, &m, 120.0, ACOUSTIC_FFT_NORMAL);
    const AcousticReport *r = last_report();
    CHECK(r->elapsed_s == 120);
    CHECK_NEAR(r->leq_total, 90.0, 0.1);
    // NR-15: 90 dB(A) -> 4 h permitidas; 120 s = 0,833 %. NHO-01: 2 h31 -> 1,323 %.
    CHECK_NEAR(r->dose_nr15_pct, 100.0 * 120.0 / 14400.0, 0.02);
    CHECK_NEAR(r->dose_nho01_pct, 100.0 * 120.0 / (28800.0 / pow(2.0, 5.0 / 3.0)), 0.03);
    CHECK_NEAR(r->l10, 90.0, 0.5);
    CHECK_NEAR(r->l90, 90.0, 0.5);
    CHECK(!(r->flags & ACOUSTIC_FLAG_DOSE_LIMIT));

    // Reinício da medição zera a dose sem perder o nível.
    CHECK(acoustic_engine_control(&g_engine, ACOUSTIC_CTRL_MEASURE_RESET, 0));
    feed(mix_signal, &m, 2.0, ACOUSTIC_FFT_NORMAL);
    r = last_report();
    CHECK(r->elapsed_s <= 2);
    CHECK(r->dose_nr15_pct < 0.02f);
    CHECK_NEAR(r->laeq, 90.0, 0.1);

    // Ln de um ruído com dois patamares: L10 no alto, L90 no baixo.
    Mix low, high;
    mix_tone(&low, 1000.0, 60.0);
    mix_tone(&high, 1000.0, 80.0);
    setup(ACOUSTIC_MODE_LEVEL, 8000.0f, 1024, 1, 8.0f);
    for (int i = 0; i < 3; ++i) {
        feed(mix_signal, &low, 10.0, ACOUSTIC_FFT_NORMAL);
        feed(mix_signal, &high, 10.0, ACOUSTIC_FFT_NORMAL);
    }
    r = last_report();
    CHECK_NEAR(r->l10, 80.0, 0.6);
    CHECK_NEAR(r->l90, 60.0, 0.6);
    CHECK(r->l50 > 59.0f && r->l50 < 81.0f);
    CHECK_NEAR(r->leq_total, 10.0 * log10((pow(10.0, 6.0) + pow(10.0, 8.0)) / 2.0), 0.2);

    // Acima de 115 dB(A) o limite é sinalizado imediatamente.
    mix_tone(&m, 1000.0, 116.0);
    setup(ACOUSTIC_MODE_LEVEL, 8000.0f, 1024, 1, 8.0f);
    feed(mix_signal, &m, 4.0, ACOUSTIC_FFT_NORMAL);
    CHECK(last_report()->flags & ACOUSTIC_FLAG_DOSE_LIMIT);
}

typedef struct {
    double background_rms;
    double start, duration;
    double burst_rms;
    double f_lo, f_hi;     // faixa do estouro: soma de senoides
    int    tonal;          // 1: uma única senoide em f_lo; 2: ruído branco
} EventScene;

static double event_signal(double t, void *ctx) {
    const EventScene *s = (const EventScene *)ctx;
    double v = s->background_rms * gauss();
    if (t >= s->start && t < s->start + s->duration) {
        if (s->tonal == 1) {
            v += s->burst_rms * sqrt(2.0) * sin(2.0 * M_PI * s->f_lo * t);
        } else if (s->tonal == 2) {
            v += s->burst_rms * gauss();
        } else {
            double b = 0.0;
            for (int i = 0; i < 16; ++i) {
                const double f = s->f_lo + (s->f_hi - s->f_lo) * i / 15.0;
                b += sin(2.0 * M_PI * f * t + i * 1.7);
            }
            v += s->burst_rms * b / sqrt(8.0);   // 16 senoides: eficaz = burst_rms
        }
    }
    return v;
}

static void run_event(EventScene *scene, double pre, double post) {
    scene->start = g_time + pre;
    feed(event_signal, scene, pre + scene->duration + post, ACOUSTIC_FFT_NORMAL);
}

static void test_event_sentinel(void) {
    setup(ACOUSTIC_MODE_EVENTS, 16000.0f, 1024, 1, 8.0f);
    EventScene scene = {pa_rms(50.0), 0, 0, 0, 0, 0, 0};
    scene.start = 1e9;
    feed(event_signal, &scene, 4.0, ACOUSTIC_FFT_NORMAL);   // aquecimento do fundo
    const AcousticReport *r = last_report();
    CHECK(r->event_count == 0);
    CHECK(r->event_age_s == ACOUSTIC_AGE_NONE);
    CHECK_NEAR(r->background_db, 50.0 + 0.0, 2.5);         // fundo branco, em dB(A) ≈ dB Z - pouco
    CHECK(r->coverage_pct < 20);                              // FFT só 1x por segundo sem eventos

    // Estalo agudo: 30 ms de ruído entre 3 e 6 kHz a 90 dB.
    scene.duration = 0.030; scene.burst_rms = pa_rms(90.0); scene.f_lo = 3000.0; scene.f_hi = 6000.0;
    run_event(&scene, 0.5, 2.5);
    r = last_report();
    CHECK(r->event_count == 1);
    CHECK(r->event_class == ACOUSTIC_EVENT_CRACK);
    CHECK(r->event_duration_ms >= 20 && r->event_duration_ms <= 120);
    CHECK(r->event_peak_db > 88.0f);
    CHECK(r->event_centroid_hz > 2500.0f);
    CHECK(!isnan(r->event_sel_db) && r->event_sel_db > 70.0f && r->event_sel_db < 85.0f);
    CHECK(r->event_age_s <= 3);

    // Impacto grave: 40 ms entre 150 e 400 Hz.
    scene.duration = 0.040; scene.burst_rms = pa_rms(88.0); scene.f_lo = 150.0; scene.f_hi = 400.0;
    run_event(&scene, 0.5, 2.5);
    r = last_report();
    CHECK(r->event_count == 2);
    CHECK(r->event_class == ACOUSTIC_EVENT_IMPACT);

    // Alarme tonal: 1,5 s de 2 kHz.
    scene.duration = 1.5; scene.burst_rms = pa_rms(75.0); scene.f_lo = 2000.0; scene.tonal = 1;
    run_event(&scene, 0.5, 2.5);
    r = last_report();
    CHECK(r->event_count == 3);
    CHECK(r->event_class == ACOUSTIC_EVENT_TONAL);
    CHECK(r->event_duration_ms > 1200 && r->event_duration_ms < 1800);

    // Passagem de banda larga: 1,5 s de ruído branco.
    scene.duration = 1.5; scene.burst_rms = pa_rms(72.0); scene.tonal = 2;
    run_event(&scene, 0.5, 2.5);
    r = last_report();
    CHECK(r->event_count == 4);
    CHECK(r->event_class == ACOUSTIC_EVENT_TRANSIENT);

    // Ruído de fundo flutuando ±1 dB não dispara.
    scene.start = 1e9;
    feed(event_signal, &scene, 5.0, ACOUSTIC_FFT_NORMAL);
    CHECK(last_report()->event_count == 4);

    CHECK(acoustic_engine_control(&g_engine, ACOUSTIC_CTRL_MEASURE_RESET, 0));
    feed(event_signal, &scene, 1.0, ACOUSTIC_FFT_NORMAL);
    CHECK(last_report()->event_count == 0);
    CHECK(last_report()->event_age_s == ACOUSTIC_AGE_NONE);
}

static void machine_mix(Mix *m, double f0, double db) {
    memset(m, 0, sizeof(*m));
    for (unsigned h = 1; h <= 8; ++h) {
        m->freq[h - 1] = f0 * h;
        m->amp[h - 1] = pa_rms(db - 3.0 * (h - 1));
    }
    m->count = 8;
    m->noise_rms = pa_rms(db - 25.0);
}

static void test_machine_signature(void) {
    Mix m;
    machine_mix(&m, 47.0, 80.0);   // motor de 4 polos a 2820 rpm
    setup(ACOUSTIC_MODE_MACHINE, 16000.0f, 2048, 1, 3.0f);
    feed(mix_signal, &m, 3.0, ACOUSTIC_FFT_NORMAL);
    const AcousticReport *r = last_report();
    CHECK_NEAR(r->f0_hz, 47.0, 0.5);
    CHECK(r->harmonics >= 5);
    CHECK(r->tonal_db > 10.0f);
    CHECK_NEAR(r->peak_hz, 47.0, 1.0);
    CHECK(r->flatness < 0.2f);
    CHECK(!(r->flags & ACOUSTIC_FLAG_BASELINE_READY));

    // Ruído branco: sem série harmônica.
    Mix noise;
    memset(&noise, 0, sizeof(noise));
    noise.noise_rms = pa_rms(70.0);
    feed(mix_signal, &noise, 2.0, ACOUSTIC_FFT_NORMAL);
    CHECK(last_report()->f0_hz == 0.0f);

    // Aprendizado só no modo MACHINE.
    setup(ACOUSTIC_MODE_LEVEL, 16000.0f, 2048, 1, 3.0f);
    CHECK(!acoustic_engine_control(&g_engine, ACOUSTIC_CTRL_BASELINE_LEARN, 10));
    setup(ACOUSTIC_MODE_MACHINE, 16000.0f, 2048, 1, 3.0f);
    CHECK(!acoustic_engine_control(&g_engine, ACOUSTIC_CTRL_BASELINE_LEARN, 2));
    CHECK(acoustic_engine_control(&g_engine, ACOUSTIC_CTRL_BASELINE_LEARN, 10));
    feed(mix_signal, &m, 5.0, ACOUSTIC_FFT_NORMAL);
    r = last_report();
    CHECK(r->flags & ACOUSTIC_FLAG_BASELINE_LEARNING);
    CHECK(r->baseline_pct == 50);
    feed(mix_signal, &m, 6.0, ACOUSTIC_FFT_NORMAL);
    r = last_report();
    CHECK(r->flags & ACOUSTIC_FLAG_BASELINE_READY);
    CHECK(!(r->flags & ACOUSTIC_FLAG_ANOMALY));
    CHECK(r->anomaly_z < 2.0f);

    // Defeito: um apito de rolamento a 3,2 kHz aparece na assinatura.
    Mix faulty = m;
    faulty.freq[faulty.count] = 3200.0;
    faulty.amp[faulty.count] = pa_rms(72.0);
    faulty.count++;
    feed(mix_signal, &faulty, 2.0, ACOUSTIC_FFT_NORMAL);
    r = last_report();
    CHECK(r->anomaly_z > 3.0f);
    CHECK(r->flags & ACOUSTIC_FLAG_ANOMALY);

    // A referência sobrevive a mudança de modo com a mesma fs e N; some se N muda.
    AcousticConfig cfg = g_engine.cfg;
    cfg.mode = ACOUSTIC_MODE_LEVEL;
    CHECK(acoustic_engine_configure(&g_engine, &cfg));
    cfg.mode = ACOUSTIC_MODE_MACHINE;
    CHECK(acoustic_engine_configure(&g_engine, &cfg));
    feed(mix_signal, &m, 2.0, ACOUSTIC_FFT_NORMAL);
    CHECK(last_report()->flags & ACOUSTIC_FLAG_BASELINE_READY);
    cfg.fft_size = 1024;
    CHECK(acoustic_engine_configure(&g_engine, &cfg));
    feed(mix_signal, &m, 2.0, ACOUSTIC_FFT_NORMAL);
    CHECK(!(last_report()->flags & ACOUSTIC_FLAG_BASELINE_READY));
    CHECK(acoustic_engine_control(&g_engine, ACOUSTIC_CTRL_BASELINE_CLEAR, 0));
}

static double constant_signal(double t, void *ctx) {
    (void)t;
    return *(const double *)ctx;
}

static void test_microphone_faults(void) {
    setup(ACOUSTIC_MODE_LEVEL, 16000.0f, 1024, 1, 8.0f);
    double zero = 0.0;
    feed(constant_signal, &zero, 2.0, ACOUSTIC_FFT_NORMAL);
    CHECK(last_report()->mic_state == ACOUSTIC_MIC_SILENT);
    CHECK(last_report()->flags & ACOUSTIC_FLAG_MIC_FAULT);
    CHECK(acoustic_engine_mic_state(&g_engine) == ACOUSTIC_MIC_SILENT);

    // SD preso em nível alto: palavras 0xFFFFFFFF (-1 após o deslocamento).
    int32_t high[256];
    for (int i = 0; i < 256; ++i) high[i] = -1;
    for (int i = 0; i < 16000 * 2 / 256; ++i) acoustic_engine_process(&g_engine, high, 256, ACOUSTIC_FFT_NORMAL);
    CHECK(acoustic_engine_mic_state(&g_engine) == ACOUSTIC_MIC_STUCK_HIGH);

    Mix m;
    mix_tone(&m, 1000.0, 60.0);
    m.noise_rms = pa_rms(30.0);
    feed(mix_signal, &m, 1.5, ACOUSTIC_FFT_NORMAL);
    CHECK(acoustic_engine_mic_state(&g_engine) == ACOUSTIC_MIC_OK);
    CHECK(!(last_report()->flags & ACOUSTIC_FLAG_MIC_FAULT));

    // Sobrecarga: senoide acima do fundo de escala (ceifada).
    mix_tone(&m, 1000.0, 122.0);
    feed(mix_signal, &m, 2.0, ACOUSTIC_FFT_NORMAL);
    CHECK(last_report()->flags & ACOUSTIC_FLAG_OVERLOAD);
    CHECK(last_report()->overload_samples > 0);
}

static void test_fft_policy_and_gaps(void) {
    Mix m;
    mix_tone(&m, 100.0, 80.0);
    setup(ACOUSTIC_MODE_LEVEL, 16000.0f, 1024, 1, 8.0f);
    feed(mix_signal, &m, 2.0, ACOUSTIC_FFT_NORMAL);
    // Sem FFT, o LZeq continua exato e o LAeq usa a última razão A/Z conhecida.
    feed(mix_signal, &m, 2.0, ACOUSTIC_FFT_NONE);
    const AcousticReport *r = last_report();
    CHECK(r->coverage_pct == 0);
    CHECK(!(r->flags & ACOUSTIC_FLAG_SPECTRUM_VALID));
    CHECK_NEAR(r->lzeq, 80.0, 0.05);
    CHECK_NEAR(r->laeq, 80.0 + acoustic_weight_a_db(100.0f), 0.3);
    CHECK(r->spectrum_bins == 0);
    // Amostras perdidas contam no tempo decorrido.
    const uint32_t before = r->elapsed_s;
    acoustic_engine_note_gap(&g_engine, 32000);
    feed(mix_signal, &m, 1.0, ACOUSTIC_FFT_NORMAL);
    CHECK(last_report()->elapsed_s >= before + 3);
    CHECK(last_report()->lost_samples == 32000);
}

static void test_configuration_limits(void) {
    acoustic_engine_init(&g_engine);
    AcousticConfig cfg = {ACOUSTIC_MODE_LEVEL, 16000.0f, 1024, 1, 8.0f, 1.0f};
    CHECK(acoustic_engine_configure(&g_engine, &cfg));
    AcousticConfig bad = cfg;
    bad.fft_size = 1000;
    CHECK(!acoustic_engine_configure(&g_engine, &bad));
    bad = cfg;
    bad.mode = 4;
    CHECK(!acoustic_engine_configure(&g_engine, &bad));
    bad = cfg;
    bad.sample_rate_hz = 96000.0f;
    CHECK(!acoustic_engine_configure(&g_engine, &bad));
    bad = cfg;
    bad.threshold = 1.0f;
    CHECK(!acoustic_engine_configure(&g_engine, &bad));
    bad = cfg;
    bad.gain = 0.0f;
    CHECK(!acoustic_engine_configure(&g_engine, &bad));
    bad = cfg;
    bad.window_type = 5;
    CHECK(!acoustic_engine_configure(&g_engine, &bad));
    CHECK(!acoustic_engine_control(&g_engine, 9, 0));

    // IDLE não produz relatórios.
    cfg.mode = ACOUSTIC_MODE_IDLE;
    CHECK(acoustic_engine_configure(&g_engine, &cfg));
    g_report_count = 0;
    Mix m;
    mix_tone(&m, 1000.0, 60.0);
    feed(mix_signal, &m, 2.0, ACOUSTIC_FFT_NORMAL);
    CHECK(g_report_count == 0);
}

static void bench(void) {
    const float rates[] = {16000.0f, 48000.0f};
    const uint16_t sizes[] = {1024, 2048};
    for (unsigned i = 0; i < 2; ++i) {
        for (unsigned j = 0; j < 2; ++j) {
            Mix m;
            machine_mix(&m, 50.0, 70.0);
            setup(ACOUSTIC_MODE_MACHINE, rates[i], sizes[j], 1, 3.0f);
            const clock_t t0 = clock();
            feed(mix_signal, &m, 10.0, ACOUSTIC_FFT_NORMAL);
            const double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC;
            printf("fs=%5.0f N=%4u: %.2f ms de CPU por segundo de áudio (inclui a síntese do sinal)\n",
                   rates[i], sizes[j], cpu * 100.0);
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        bench();
        return 0;
    }
    test_weighting_curves();
    test_fft_against_dft();
    test_calibration_1khz_94db();
    test_frequency_weighting_in_levels();
    test_broadband_noise();
    test_dose_and_statistics();
    test_event_sentinel();
    test_machine_signature();
    test_microphone_faults();
    test_fft_policy_and_gaps();
    test_configuration_limits();
    if (g_failures) {
        fprintf(stderr, "acoustic_dsp: %d falha(s)\n", g_failures);
        return 1;
    }
    printf("acoustic_dsp native tests: PASS\n");
    return 0;
}
