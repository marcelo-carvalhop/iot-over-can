/**
 * @file dsp_pipeline.h
 * @brief Prototipação do motor matemático DSP e algorítmico da borda.
 */
#ifndef DSP_PIPELINE_H
#define DSP_PIPELINE_H

#include <stdint.h>
#include <stdbool.h>
#include "edge_protocol_definitions.h"

#define DSP_BUFFER_MAX_SAMPLES 512
#define DSP_BUFFER_SAMPLES DSP_BUFFER_MAX_SAMPLES
#define DSP_BUFFER_MIN_SAMPLES 128

typedef enum {
    WIN_RECTANGULAR  = 0,
    WIN_HANN         = 1,
    WIN_HAMMING      = 2,
    WIN_FLATTOP      = 3,
    WIN_BLACKMAN_H   = 4
} WindowType;

typedef struct {
    float rms_ac;             // Item 5: Valor Efetivo dinâmico (m/s^2)
    float kurtosis;           // Item 11: CURTOSE EM EXCESSO (kurtosis - 3.0); gaussiano ~0.0
    float crest_factor;       // Item 6: Razão Pico/RMS (SHM)
    float peak_freq_hz;       // Item 4: Frequência dominante da máquina (0 se modo não usa FFT)
    float peak_amplitude;     // Amplitude com correção de janela (0 se modo não usa FFT)
    float spectral_entropy;   // Item 6: Entropia de Shannon (0.0 a 1.0; só em FSM_MODE_STRUCTURAL)
    bool  clipping_detected;  // Item 9: Saturação mecânica do ADC (repassado por quem chama)
    bool  seismic_triggered;  // Item 7: Alerta do filtro STA/LTA (só em FSM_MODE_SEISMIC_STALTA)
    float ppv_max_mm_s;       // Item 8: Pico de Velocidade de Partícula (Eng. Forense)
    uint8_t axis_mask;        // AXIS_MASK_VECTOR indica fusão tri-axial por magnitude
    uint16_t window_size;     // quantidade de amostras efetivamente processadas
    bool fft_valid;           // false quando o modo não executa FFT
} DSP_AnalysisResult;

void  dsp_init_engine(void);
void  dsp_configure_window_lut(WindowType win_type);
// Thread/IRQ-safe: apenas enfileira a config recebida da rede OU do
// console serial. Quem efetivamente aplica é dsp_apply_pending_config(),
// chamada do loop principal.
void  dsp_apply_remote_config(const Payload_Configuration *cfg);
// Deve ser chamada pelo loop principal a cada iteração, fora de qualquer
// pipeline em andamento. Se havia config pendente: aplica atomicamente os
// campos de domínio DSP (janela, limiar STA/LTA, ganho de calibração),
// devolve uma cópia completa em *out_cfg (pode ser NULL se o chamador não
// precisar dos campos de sistema) e retorna true. Se não havia nada
// pendente, retorna false e não escreve em *out_cfg. Campos de sistema
// (target_mode, sample_rate_hz) devem ser aplicados pelo CHAMADOR a partir
// de *out_cfg — dsp_pipeline não tem acesso a g_active_fsm nem ao driver
// do MPU.
bool  dsp_apply_pending_config(Payload_Configuration *out_cfg);
// Setters/getters individuais — usados pelo console serial para permitir
// ajuste de um campo por vez antes de um "APPLY" (que monta um
// Payload_Configuration completo e chama dsp_apply_remote_config).
void       dsp_set_stalta_threshold(float threshold);
void       dsp_set_calibration_gain(float gain);
WindowType dsp_get_current_window(void);
float      dsp_get_stalta_threshold(void);
float      dsp_get_calibration_gain(void);
void       dsp_set_active_window_size(uint16_t n);
uint16_t   dsp_get_active_window_size(void);
bool       dsp_is_last_fft_valid(void);
// mode controla quais estágios rodam (ver comentário no .c); clipping_detected
// é repassado pelo chamador (calculado no driver do MPU) e apenas ecoado no
// resultado.
void  dsp_run_vibration_pipeline(float *time_data_x, float *time_data_y, float *time_data_z,
                                 float sample_rate_hz, OperationModeFSM mode, 
                                 bool clipping_detected,
                                 DSP_AnalysisResult *result, float *out_mag_spectrum);
bool  dsp_process_seismic_stalta(float sample_z, float fs_hz);
// Item 12: consulta o estado ATUAL (persistente entre buffers) do detector
// STA/LTA sem reprocessar nada. Útil para decisões que precisam rodar
// ANTES do pipeline (que destrói time_data_z in-place via FFT), como a
// checagem de sanidade gravitacional em main.c.
bool  dsp_is_seismic_currently_triggered(void);

#endif // DSP_PIPELINE_H