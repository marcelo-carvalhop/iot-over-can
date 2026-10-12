/**
 * @file acoustic_dsp.h
 * @brief Motor de análise acústica do sensor INMP441: níveis, dose, assinatura e eventos.
 *
 * Código C puro, sem dependência do Pico SDK: o mesmo arquivo roda no núcleo 1
 * do RP2040/RP2350 e nos testes do computador (Codigo/node-audio/tests).
 *
 * Entrada: palavras I2S de 32 bits do INMP441 (dado de 24 bits alinhado à
 * esquerda), em qualquer quantidade por chamada. O tempo é contado em
 * amostras; nada depende de relógio.
 *
 * Organização em três escalas de tempo:
 *
 *   amostra      acumuladores inteiros (soma, soma dos quadrados, pico) e cópia
 *                para o quadro da FFT. Sem perda de amostras: é a base do LZeq.
 *   subquadro    10 ms. Energia do subquadro, ponderações temporais F e S,
 *                histograma de LAF, dose e detector STA/LTA.
 *   quadro FFT   N amostras (256..2048). Espectro de potência, energias com
 *                ponderação A e C, média de Welch, curtose. Pode ser pulado
 *                quando o processador não acompanha (cobertura < 100 %).
 *   relatório    1 s. Fecha os níveis do intervalo e produz AcousticReport.
 *
 * LAeq e LCeq usam a energia exata do domínio do tempo multiplicada pela
 * razão E_A/E_Z medida nos quadros analisados pela FFT. A correção do
 * janelamento cancela na razão, e o nível absoluto continua sem lacunas
 * mesmo quando nem todos os quadros passam pela FFT.
 */
#ifndef ACOUSTIC_DSP_H
#define ACOUSTIC_DSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_fft.h"

#define ACOUSTIC_MAX_FFT            AUDIO_FFT_MAX_N
#define ACOUSTIC_SPECTRUM_OUT_BINS  256     // faixas do espectro enviado ao Node
#define ACOUSTIC_BANDS              10      // oitavas de 31,5 Hz a 16 kHz
#define ACOUSTIC_LN_BIN_DB          0.5f    // resolução do histograma de LAF
#define ACOUSTIC_LN_BINS            280     // 0 a 140 dB
#define ACOUSTIC_SUBFRAMES_PER_S    100     // subquadro de 10 ms

// Sensibilidade nominal do INMP441: -26 dBFS a 94 dB SPL, com 0 dBFS = senoide
// de fundo de escala (convenção InvenSense, AN-1112). Uma amostra normalizada
// x = s / 2^23 corresponde a x · 28,2843 Pa; a senoide de fundo de escala
// (20 Pa eficazes) é o ponto de sobrecarga acústica, 120 dB SPL.
#define ACOUSTIC_PA_PER_FULL_SCALE  28.2843f
#define ACOUSTIC_FULL_SCALE_COUNTS  8388608.0f   // 2^23
#define ACOUSTIC_P0_PA              20e-6f       // referência de 0 dB SPL
#define ACOUSTIC_CLIP_COUNTS        8388000      // |s| a partir daqui: amostra saturada

// Política de FFT de cada chamada, decidida pelo núcleo 1 conforme a fila de DMA.
typedef enum {
    ACOUSTIC_FFT_NORMAL = 0,   // analisa os quadros que o modo pede
    ACOUSTIC_FFT_LOADED = 1,   // atrasado: só quadros de evento
    ACOUSTIC_FFT_NONE   = 2    // muito atrasado: nenhuma FFT
} AcousticFftPolicy;

typedef enum {
    ACOUSTIC_MIC_OK         = 0,
    ACOUSTIC_MIC_SILENT     = 1,   // todas as palavras em zero por 1 s: SD aberto ou microfone desligado
    ACOUSTIC_MIC_STUCK_HIGH = 2    // todas as palavras em 1 por 1 s: SD preso em nível alto
} AcousticMicState;

typedef struct {
    uint8_t  mode;             // AcousticModeFSM (edge_protocol_definitions.h)
    float    sample_rate_hz;   // taxa efetiva da PIO
    uint16_t fft_size;         // 256, 512, 1024 ou 2048
    uint8_t  window_type;      // 0 RECT, 1 HANN, 2 HAMMING, 3 FLATTOP, 4 BLACKMAN-HARRIS
    float    threshold;        // EVENTS: razão STA/LTA; MACHINE: escore z de anomalia
    float    gain;             // ganho de calibração da pressão
} AcousticConfig;

// Relatório de um intervalo (1 s). Valores em dB re 20 µPa; NAN = não calculado.
typedef struct {
    uint8_t  mode;
    uint8_t  flags;               // ACOUSTIC_FLAG_* (edge_protocol_definitions.h)
    uint16_t fft_size;
    float    sample_rate_hz;
    uint32_t samples;             // amostras integradas no intervalo
    uint8_t  coverage_pct;        // quadros analisados / quadros completados
    float    laeq, lceq, lzeq, lzpeak;
    float    laf, lafmax, lafmin;
    float    crest;               // pico / eficaz, sem ponderação
    float    kurtosis;            // excesso de curtose (0 = gaussiano)
    float    peak_hz, centroid_hz, flatness;
    // LEVEL (desde o último reinício)
    float    leq_total, l10, l50, l90;
    float    dose_nr15_pct, dose_nho01_pct;
    float    las_max;             // maior LAS do intervalo
    uint32_t elapsed_s;
    // MACHINE
    float    tonal_db, f0_hz;
    uint8_t  harmonics;
    float    anomaly_z;
    uint8_t  baseline_pct;
    // EVENTS
    uint16_t event_count;
    uint8_t  events_interval;
    uint8_t  event_class;         // AcousticEventClass
    float    event_peak_db, event_sel_db, event_centroid_hz;
    uint16_t event_duration_ms;
    uint16_t event_age_s;         // 0xFFFF = nenhum evento
    float    sta_lta_db, background_db;
    // Bandas de oitava (Z) e espectro médio do intervalo
    float    octave_db[ACOUSTIC_BANDS];
    uint16_t spectrum_bins;
    float    spectrum_pa[ACOUSTIC_SPECTRUM_OUT_BINS];  // pressão eficaz por faixa
    // Diagnóstico
    uint8_t  mic_state;           // AcousticMicState
    uint32_t overload_samples;
    uint32_t lost_samples;        // amostras perdidas por estouro do anel de DMA
} AcousticReport;

typedef struct {
    // Configuração e constantes derivadas
    AcousticConfig cfg;
    bool     configured;
    float    pa_per_count;        // pressão por unidade de 24 bits, com o ganho
    uint32_t subframe_len;        // amostras por subquadro (fs / 100)
    uint32_t report_len;          // amostras por relatório (fs)
    float    t_sub;               // duração do subquadro (s)
    float    alpha_fast, alpha_slow, alpha_sta, alpha_lta;
    float    window_ms;           // média de w² (normalização do espectro)

    // Amostra -> subquadro
    int64_t  sub_sum, sub_sumsq;
    int32_t  sub_peak;
    int32_t  sub_or, sub_and;
    uint32_t sub_n;
    double   dc;                  // offset estimado (contagens)
    bool     dc_valid;

    // Subquadro -> relatório
    double   rep_energy_z;        // Σ Pa²·amostra (sem ponderação)
    uint32_t rep_n;
    int32_t  rep_peak;
    uint32_t rep_overload;
    float    laf_max, laf_min, las_max;
    uint32_t rep_lost;

    // Ponderações temporais e razões espectrais
    float    e_fast, e_slow;      // energia A ponderada no tempo (Pa²)
    bool     tw_valid;
    float    ratio_a, ratio_c;    // E_A/E_Z do quadro mais recente
    bool     ratio_valid;

    // Quadro FFT, com buffer duplo: o quadro anterior fica guardado para que o
    // início de um evento, caído num quadro que fechou sem FFT, seja analisado
    // no instante do disparo.
    int32_t  frame[2][ACOUSTIC_MAX_FFT];
    uint8_t  frame_cur;
    bool     prev_pending;        // quadro anterior completo e ainda não analisado
    uint32_t frame_fill;
    uint32_t frames_since_fft;
    uint8_t  policy;              // AcousticFftPolicy da chamada em curso
    float    work[ACOUSTIC_MAX_FFT];
    float    power[ACOUSTIC_MAX_FFT / 2 + 1];
    float    window[ACOUSTIC_MAX_FFT];
    float    weight_a[ACOUSTIC_MAX_FFT / 2 + 1];
    float    weight_c[ACOUSTIC_MAX_FFT / 2 + 1];
    float    welch[ACOUSTIC_MAX_FFT / 2 + 1];
    uint32_t welch_count;
    double   rep_fft_ez, rep_fft_ea, rep_fft_ec;
    double   rep_m2, rep_m4;
    uint32_t rep_kurt_n;
    uint32_t rep_frames_done, rep_frames_fft;
    AudioFft fft;

    // LEVEL
    uint32_t ln_hist[ACOUSTIC_LN_BINS];
    uint32_t ln_count;
    double   dose_nr15, dose_nho01;   // frações (1,0 = 100 %)
    double   energy_a_total;          // Σ Pa²·s com ponderação A
    double   elapsed_samples;

    // MACHINE: assinatura de referência por banda de oitava (Welford)
    uint32_t base_n[ACOUSTIC_BANDS];
    double   base_mean[ACOUSTIC_BANDS];
    double   base_m2[ACOUSTIC_BANDS];
    uint16_t learn_total_s, learn_left_s;
    bool     baseline_ready;

    // EVENTS
    float    sta, lta;
    bool     sl_valid;
    uint32_t warmup_sub;          // subquadros até o detector ficar armado
    bool     ev_active;
    uint32_t ev_sub_count;        // subquadros desde o disparo
    uint32_t ev_last_above;       // último subquadro acima de 2 × fundo
    double   ev_excess;           // Σ (e - fundo)·t_sub (Pa²·s)
    int32_t  ev_peak;
    double   ev_fft_ez, ev_fft_ea, ev_sum_fp, ev_sum_p, ev_flat_sum, ev_flat_w;
    uint16_t ev_count;
    uint8_t  ev_interval;
    uint8_t  ev_last_class;
    float    ev_last_peak_db, ev_last_sel_db, ev_last_centroid;
    uint16_t ev_last_duration_ms;
    double   ev_last_end_samples; // instante do fim do último evento (amostras desde o reinício)
    bool     ev_any;

    // Microfone
    uint32_t silent_sub, high_sub;
    uint8_t  mic_state;

    // Relatório pronto
    bool     report_ready;
    AcousticReport report;
} AcousticEngine;

void  acoustic_engine_init(AcousticEngine *e);

/** Valida e aplica a configuração. Reinicia medição; a assinatura de referência só é descartada se fs ou N mudarem. */
bool  acoustic_engine_configure(AcousticEngine *e, const AcousticConfig *cfg);

/** Ações de AcousticControlAction. Retorna false para ação ou argumento inválido. */
bool  acoustic_engine_control(AcousticEngine *e, uint8_t action, uint16_t argument);

/** Processa `count` palavras I2S (dado de 24 bits nos bits 31..8). */
void  acoustic_engine_process(AcousticEngine *e, const int32_t *words, size_t count, AcousticFftPolicy policy);

/** Amostras perdidas (estouro do anel de DMA): o tempo avança com o último nível conhecido. */
void  acoustic_engine_note_gap(AcousticEngine *e, uint32_t lost_samples);

/** Copia o relatório pronto, se houver, e o consome. */
bool  acoustic_engine_take_report(AcousticEngine *e, AcousticReport *out);

/** Estado do microfone na última análise (para a troca automática de canal). */
uint8_t acoustic_engine_mic_state(const AcousticEngine *e);

// Utilitários puros, expostos para os testes.
float acoustic_weight_a_db(float f_hz);
float acoustic_weight_c_db(float f_hz);
float acoustic_band_center_hz(unsigned band);   // nominal: 31,5 .. 16000 Hz

#endif /* ACOUSTIC_DSP_H */
