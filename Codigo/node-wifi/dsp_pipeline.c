/**
 * @file dsp_pipeline.c
 * @brief Implementação dos algoritmos matemáticos: Janela de Hann, FFT Radix-2 in-place,
 * RMS AC desacoplado, Curtose temporal, Entropia Espectral e STA/LTA (Sismografia).
 * Atende aos Itens 3, 4, 5, 6, 7, 8, 11.
 */
#include "dsp_pipeline.h"
#include "config_validation.h"
#include <math.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// Tabela Look-Up (LUT) para a função de janela (2 KB na SRAM)
static float g_window_lut[DSP_BUFFER_MAX_SAMPLES];
static WindowType g_current_window = WIN_HANN;
static uint16_t g_active_window_size = DSP_BUFFER_MAX_SAMPLES;
static bool g_last_fft_valid = false;

// Parâmetros para o Sismógrafo STA/LTA (Item 7 e 8)
typedef struct {
    float sta;
    float lta;
    float alpha_sta;
    float alpha_lta;
    float trigger_threshold;
    float detrigger_ratio;
    bool  is_triggered;
    float configured_fs_hz; // fs_hz usado para calcular alpha_sta/alpha_lta pela última vez
} SeismicDetector;

static SeismicDetector g_sismo_det;

// ----------------------------------------------------------------------
// CONFIGURAÇÃO REMOTA ATÔMICA (correção de corrida de dados)
// ----------------------------------------------------------------------
// dsp_apply_remote_config() é chamada a partir do callback de rede lwIP,
// que roda em um contexto assíncrono à parte (async_context de baixa
// prioridade da arquitetura threadsafe_background) e pode preemptar o
// loop principal a qualquer momento — inclusive no meio de
// dsp_run_vibration_pipeline(). Gravar g_current_window/g_window_lut/
// trigger_threshold diretamente ali, enquanto o pipeline os lê no loop
// principal, podia produzir uma FFT calculada com janela parcialmente
// trocada, ou um limiar de STA/LTA mudando no meio do cálculo.
//
// Correção: o callback de rede só grava numa cópia "pendente" da config;
// dsp_apply_pending_config() deve ser chamada pelo loop principal, no
// início de cada iteração (fora de qualquer pipeline em andamento), onde
// aplica a configuração de forma atômica.
static Payload_Configuration g_pending_cfg;
static volatile bool         g_cfg_dirty = false;

// Item (menu de configuração / calib_gain nunca aplicado): o campo
// calib_gain do Payload_Configuration era aceito no pacote mas nunca usado
// em lugar nenhum. Agora é multiplicado no eixo Z logo no início do
// pipeline (ver dsp_run_vibration_pipeline), com 1.0f como neutro.
static float g_calib_gain = 1.0f;

void dsp_init_engine(void) {
    dsp_configure_window_lut(WIN_HANN);
    // Configuração inicial do STA/LTA (Janela curta 0.5s, Longa 30s).
    // Os alphas dependem da taxa de amostragem REAL de chamada (fs_hz) e são
    // (re)calculados dentro de dsp_process_seismic_stalta() na 1a execução e
    // sempre que fs_hz mudar (ver g_sismo_det.configured_fs_hz).
    g_sismo_det.sta = 0.0f;
    g_sismo_det.lta = 1e-6f;
    g_sismo_det.alpha_sta = 0.0f;
    g_sismo_det.alpha_lta = 0.0f;
    g_sismo_det.trigger_threshold = 4.0f;
    g_sismo_det.detrigger_ratio   = 1.5f;
    g_sismo_det.is_triggered      = false;
    g_sismo_det.configured_fs_hz  = 0.0f;
    g_cfg_dirty = false;
    g_calib_gain = 1.0f;
    g_active_window_size = DSP_BUFFER_MAX_SAMPLES;
    g_last_fft_valid = false;
}

// Chamada em contexto de rede OU do console serial: apenas copia a config
// recebida para o buffer pendente. Não aplica nada diretamente (ver nota
// de corrida de dados acima).
void dsp_apply_remote_config(const Payload_Configuration *cfg) {
    /* Defense in depth: both serial and network validate at ingress, but this
       module refuses an invalid structure if a future caller bypasses them. */
    if (!edge_config_validate(cfg, NULL)) return;
    uint32_t irq_state = save_and_disable_interrupts();
    g_pending_cfg = *cfg;
    g_cfg_dirty = true;
    restore_interrupts(irq_state);
}

void dsp_set_stalta_threshold(float threshold) {
    if (threshold > 1.0f) g_sismo_det.trigger_threshold = threshold;
}

void dsp_set_calibration_gain(float gain) {
    if (gain > 0.0f) g_calib_gain = gain;
}

WindowType dsp_get_current_window(void)    { return g_current_window; }
float      dsp_get_stalta_threshold(void)  { return g_sismo_det.trigger_threshold; }
float      dsp_get_calibration_gain(void)  { return g_calib_gain; }

static bool dsp_is_supported_window_size(uint16_t n) {
    return (n == 128 || n == 256 || n == 512);
}

void dsp_set_active_window_size(uint16_t n) {
    if (dsp_is_supported_window_size(n)) {
        g_active_window_size = n;
        dsp_configure_window_lut(g_current_window);
    }
}

uint16_t dsp_get_active_window_size(void) { return g_active_window_size; }
bool dsp_is_last_fft_valid(void) { return g_last_fft_valid; }

// Chamada pelo loop principal (main.c), fora de qualquer pipeline em
// andamento: se houver config pendente, aplica atomicamente os campos que
// são domínio deste módulo (janela, limiar STA/LTA, ganho de calibração) e
// devolve uma cópia completa em *out_cfg para que o chamador trate os
// campos de domínio "de sistema" (target_mode, sample_rate_hz) — únicos
// que main.c/mpu6050_dma_driver conseguem aplicar. Retorna true se havia
// config pendente (e portanto *out_cfg foi preenchido).
bool dsp_apply_pending_config(Payload_Configuration *out_cfg) {
    Payload_Configuration local_cfg;
    bool has_pending;

    uint32_t irq_state = save_and_disable_interrupts();
    has_pending = g_cfg_dirty;
    if (has_pending) {
        local_cfg = g_pending_cfg;
        g_cfg_dirty = false;
    }
    restore_interrupts(irq_state);

    if (!has_pending) return false;

    if (dsp_is_supported_window_size(local_cfg.window_size)) {
        dsp_set_active_window_size(local_cfg.window_size);
    }
    if (local_cfg.window_type <= WIN_BLACKMAN_H) {
        dsp_configure_window_lut((WindowType)local_cfg.window_type);
    }
    dsp_set_stalta_threshold(local_cfg.stalta_thresh);
    dsp_set_calibration_gain(local_cfg.calib_gain);

    if (out_cfg) *out_cfg = local_cfg;
    return true;
}

// ITEM 3: JANELA DE HANN E DEMAIS JANELAS ESPECTRAIS
void dsp_configure_window_lut(WindowType win_type) {
    g_current_window = win_type;
    for (int n = 0; n < g_active_window_size; n++) {
        float ratio = (float)n / (float)(g_active_window_size - 1);
        switch (win_type) {
            case WIN_RECTANGULAR:
                g_window_lut[n] = 1.0f;
                break;
            case WIN_HANN: // w[n] = 0.5 * (1 - cos(2*pi*n / (N-1)))
                g_window_lut[n] = 0.5f * (1.0f - cosf(2.0f * M_PI * ratio));
                break;
            case WIN_HAMMING:
                g_window_lut[n] = 0.54f - 0.46f * cosf(2.0f * M_PI * ratio);
                break;
            case WIN_FLATTOP:
                g_window_lut[n] = 0.215578f - 0.416631f * cosf(2.0f * M_PI * ratio) 
                                + 0.277263f * cosf(4.0f * M_PI * ratio) 
                                - 0.083579f * cosf(6.0f * M_PI * ratio) 
                                + 0.006947f * cosf(8.0f * M_PI * ratio);
                break;
            case WIN_BLACKMAN_H:
                g_window_lut[n] = 0.35875f - 0.48829f * cosf(2.0f * M_PI * ratio) 
                                + 0.14128f * cosf(4.0f * M_PI * ratio) 
                                - 0.01168f * cosf(6.0f * M_PI * ratio);
                break;
            default:
                g_window_lut[n] = 1.0f;
                break;
        }
    }
}

static inline void dsp_apply_window(float *data) {
    if (g_current_window == WIN_RECTANGULAR) return;
    for (int n = 0; n < g_active_window_size; n++) {
        data[n] *= g_window_lut[n];
    }
}

// ITEM 4: TRANSFORMADA RÁPIDA DE FOURIER (FFT RADIX-2 IN-PLACE)
static void dsp_bit_reversal(float *real, float *imag, uint16_t n) {
    uint16_t j = 0;
    for (uint16_t i = 0; i < n - 1; i++) {
        if (i < j) {
            float temp_r = real[i];
            float temp_i = imag[i];
            real[i] = real[j];
            imag[i] = imag[j];
            real[j] = temp_r;
            imag[j] = temp_i;
        }
        uint16_t k = n >> 1;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }
}

static void dsp_fft_radix2(float *real, float *imag, uint16_t n) {
    dsp_bit_reversal(real, imag, n);
    for (uint16_t len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * M_PI / (float)len;
        float wlen_r = cosf(angle);
        float wlen_i = sinf(angle);
        for (uint16_t i = 0; i < n; i += len) {
            float w_r = 1.0f;
            float w_i = 0.0f;
            uint16_t half_len = len >> 1;
            for (uint16_t j = 0; j < half_len; j++) {
                uint16_t u_idx = i + j;
                uint16_t v_idx = i + j + half_len;
                float v_r = real[v_idx] * w_r - imag[v_idx] * w_i;
                float v_i = real[v_idx] * w_i + imag[v_idx] * w_r;
                real[v_idx] = real[u_idx] - v_r;
                imag[v_idx] = imag[u_idx] - v_i;
                real[u_idx] += v_r;
                imag[u_idx] += v_i;
                float next_w_r = w_r * wlen_r - w_i * wlen_i;
                float next_w_i = w_r * wlen_i + w_i * wlen_r;
                w_r = next_w_r;
                w_i = next_w_i;
            }
        }
    }
}

// ITEM 6: ENTROPIA ESPECTRAL DE SHANNON (SHM)
static float dsp_calculate_spectral_entropy(const float *mag_spectrum, uint16_t half_n) {
    if (half_n == 0) return 0.0f;
    float total_energy = 0.0f;
    for (uint16_t i = 0; i < half_n; i++) {
        total_energy += mag_spectrum[i];
    }
    if (total_energy <= 1e-6f) return 0.0f;
    float entropy = 0.0f;
    for (uint16_t i = 0; i < half_n; i++) {
        float p_i = mag_spectrum[i] / total_energy;
        if (p_i > 1e-12f) {
            entropy -= p_i * logf(p_i);
        }
    }
    return entropy / logf((float)half_n); // Normalizado entre 0.0 e 1.0
}

// ITEM 7: SISMOLOGIA DE FORTE MOVIMENTO (FILTRO STA/LTA)
// IMPORTANTE: esta função deve ser chamada UMA VEZ POR AMOSTRA (a fs_hz), não
// uma vez por buffer. Os alphas são recalculados automaticamente sempre que
// fs_hz mudar, evitando janelas de tempo erradas (bug anterior: alphas fixos
// assumiam 62.5 Hz, mas a função era chamada a ~1.95 Hz por buffer de 512).
bool dsp_process_seismic_stalta(float sample_z, float fs_hz) {
    if (fs_hz > 0.0f && fs_hz != g_sismo_det.configured_fs_hz) {
        g_sismo_det.alpha_sta = 1.0f / (0.5f  * fs_hz); // janela curta: 0.5 s
        g_sismo_det.alpha_lta = 1.0f / (30.0f * fs_hz);  // janela longa: 30 s
        g_sismo_det.configured_fs_hz = fs_hz;
    }
    float energy = sample_z * sample_z;
    g_sismo_det.sta = (g_sismo_det.alpha_sta * energy) + ((1.0f - g_sismo_det.alpha_sta) * g_sismo_det.sta);
    if (!g_sismo_det.is_triggered) {
        g_sismo_det.lta = (g_sismo_det.alpha_lta * energy) + ((1.0f - g_sismo_det.alpha_lta) * g_sismo_det.lta);
    }
    float ratio = g_sismo_det.sta / g_sismo_det.lta;
    if (!g_sismo_det.is_triggered && (ratio > g_sismo_det.trigger_threshold)) {
        g_sismo_det.is_triggered = true;
    } else if (g_sismo_det.is_triggered && (ratio < g_sismo_det.detrigger_ratio)) {
        g_sismo_det.is_triggered = false;
    }
    return g_sismo_det.is_triggered;
}

bool dsp_is_seismic_currently_triggered(void) {
    return g_sismo_det.is_triggered;
}

// ITEM 11: SEQUÊNCIA CANÔNICA DE PIPELINE DSP
//
// Item 13 (custo de CPU por modo): o header original documenta que cada
// FSM_MODE_* precisa de um subconjunto de métricas, mas a implementação
// anterior sempre calculava tudo (inclusive a FFT, de longe o estágio mais
// caro). Agora o pipeline liga/desliga estágios conforme `mode`:
//   - FSM_MODE_IDLE: retorna imediatamente com resultado zerado (defensivo;
//     em condições normais o loop principal não deveria chamar o pipeline
//     nesse modo, pois a aquisição DMA está pausada).
//   - FSM_MODE_ROTATING_MACH: RMS/crest/curtose (sempre baratos) + FFT/pico.
//     Sem entropia espectral, sem STA/LTA.
//   - FSM_MODE_STRUCTURAL: RMS/crest/curtose + FFT/entropia espectral.
//     Sem STA/LTA.
//   - FSM_MODE_SEISMIC_STALTA: RMS/crest/curtose + STA/LTA amostra-a-amostra.
//     SEM FFT/janelamento/entropia (não fazem parte do contrato desse modo
//     e são o estágio mais caro do pipeline).
// DECISÃO DE DESIGN: alternar para fora de FSM_MODE_SEISMIC_STALTA pausa a
// atualização do detector STA/LTA (ele não "recebe" amostras enquanto o nó
// está, por exemplo, em modo estrutural). Se for necessário monitoramento
// sísmico contínuo simultâneo a outros modos, isso precisa virar um segundo
// pipeline paralelo — fora do escopo desta correção.
void dsp_run_vibration_pipeline(float *time_data_x, float *time_data_y, float *time_data_z,
                                 float sample_rate_hz, OperationModeFSM mode,
                                 bool clipping_detected,
                                 DSP_AnalysisResult *result, float *out_mag_spectrum) {
    uint16_t n = g_active_window_size;
    if (n < DSP_BUFFER_MIN_SAMPLES || n > DSP_BUFFER_MAX_SAMPLES) n = DSP_BUFFER_MAX_SAMPLES;

    memset(result, 0, sizeof(*result));
    result->clipping_detected = clipping_detected;
    result->axis_mask = AXIS_MASK_VECTOR;
    result->window_size = n;
    result->fft_valid = false;
    g_last_fft_valid = false;

    if (out_mag_spectrum) {
        memset(out_mag_spectrum, 0, (DSP_BUFFER_MAX_SAMPLES / 2) * sizeof(float));
    }

    if (mode == FSM_MODE_IDLE) {
        return;
    }

    bool need_fft     = (mode == FSM_MODE_ROTATING_MACH || mode == FSM_MODE_STRUCTURAL);
    bool need_entropy = (mode == FSM_MODE_STRUCTURAL);
    bool need_stalta  = (mode == FSM_MODE_SEISMIC_STALTA);

    // O pipeline agora processa os três eixos. Para manter uma única série
    // escalar compatível com FFT/STA-LTA, usa a magnitude vetorial tri-axial.
    // Isso elimina a limitação anterior de considerar somente Z.
    static float scalar[DSP_BUFFER_MAX_SAMPLES];
    for (uint16_t i = 0; i < n; i++) {
        float x = time_data_x ? time_data_x[i] : 0.0f;
        float y = time_data_y ? time_data_y[i] : 0.0f;
        float z = time_data_z ? time_data_z[i] : 0.0f;
        scalar[i] = sqrtf(x * x + y * y + z * z) * g_calib_gain;
    }

    float sum = 0.0f;
    for (uint16_t i = 0; i < n; i++) sum += scalar[i];
    float mean_dc = sum / (float)n;

    float sum_sq = 0.0f;
    float max_peak = 0.0f;
    for (uint16_t i = 0; i < n; i++) {
        scalar[i] -= mean_dc;
        float abs_val = fabsf(scalar[i]);
        if (abs_val > max_peak) max_peak = abs_val;
        sum_sq += scalar[i] * scalar[i];
    }

    result->rms_ac = sqrtf(sum_sq / (float)n);
    result->crest_factor = (result->rms_ac > 1e-5f) ? (max_peak / result->rms_ac) : 0.0f;

    bool seismic_trig_in_buffer = false;
    if (need_stalta) {
        for (uint16_t i = 0; i < n; i++) {
            if (dsp_process_seismic_stalta(scalar[i], sample_rate_hz)) {
                seismic_trig_in_buffer = true;
            }
        }
    }
    result->seismic_triggered = seismic_trig_in_buffer;

    float dt = 1.0f / sample_rate_hz;
    float max_vel = 0.0f;
    float vel_accum = 0.0f;
    for (uint16_t i = 0; i < n; i++) {
        vel_accum += scalar[i] * dt * 1000.0f;
        if (fabsf(vel_accum) > max_vel) max_vel = fabsf(vel_accum);
    }
    result->ppv_max_mm_s = max_vel;

    float sum_fourth = 0.0f;
    for (uint16_t i = 0; i < n; i++) {
        float val_sq = scalar[i] * scalar[i];
        sum_fourth += val_sq * val_sq;
    }
    float variance = sum_sq / (float)n;
    float raw_kurtosis = (variance > 1e-5f) ? ((sum_fourth / (float)n) / (variance * variance)) : 3.0f;
    result->kurtosis = raw_kurtosis - 3.0f;

    if (!need_fft) {
        return;
    }

    dsp_apply_window(scalar);

    static float imag_buffer[DSP_BUFFER_MAX_SAMPLES];
    memset(imag_buffer, 0, sizeof(imag_buffer));
    dsp_fft_radix2(scalar, imag_buffer, n);

    uint16_t half_n = n / 2;
    float max_mag = 0.0f;
    uint16_t peak_bin = 0;

    for (uint16_t i = 0; i < half_n; i++) {
        float mag = sqrtf(scalar[i] * scalar[i] + imag_buffer[i] * imag_buffer[i]) / (float)n;
        if (i > 0) mag *= 2.0f;
        if (out_mag_spectrum) out_mag_spectrum[i] = mag;
        if (i > 0 && mag > max_mag) {
            max_mag = mag;
            peak_bin = i;
        }
    }

    float delta_f = sample_rate_hz / (float)n;
    result->peak_freq_hz   = (float)peak_bin * delta_f;
    result->peak_amplitude = max_mag;
    result->fft_valid = true;
    g_last_fft_valid = true;

    if (need_entropy && out_mag_spectrum) {
        result->spectral_entropy = dsp_calculate_spectral_entropy(out_mag_spectrum, half_n);
    }
}
