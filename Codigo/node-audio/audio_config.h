/**
 * @file audio_config.h
 * @brief Faixas de configuração do sensor acústico e conversão para o motor DSP.
 *
 * audio_config.c implementa a API de config_validation.h (edge_config_validate
 * e afins) com as faixas do INMP441; o driver de rede compartilhado chama essas
 * funções sem saber qual sensor está compilado.
 */
#ifndef AUDIO_CONFIG_H
#define AUDIO_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "acoustic_dsp.h"
#include "config_validation.h"
#include "edge_protocol_definitions.h"

// WS do INMP441 de 7,8 a 50 kHz; SCK = 64 · fs.
#define AUDIO_RATE_COUNT 5
extern const uint32_t AUDIO_SAMPLE_RATES_HZ[AUDIO_RATE_COUNT];   // 8000, 16000, 24000, 32000, 48000
#define AUDIO_FFT_SIZE_COUNT 4
extern const uint16_t AUDIO_FFT_SIZES[AUDIO_FFT_SIZE_COUNT];     // 256, 512, 1024, 2048

#define AUDIO_THRESHOLD_MIN_EXCLUSIVE 1.0f
#define AUDIO_THRESHOLD_MAX           1000.0f
#define AUDIO_GAIN_MIN_EXCLUSIVE      0.01f    // ±40 dB em torno da sensibilidade nominal
#define AUDIO_GAIN_MAX                100.0f
#define AUDIO_DEFAULT_THRESHOLD_EVENTS  8.0f   // STA/LTA: +9 dB sobre o fundo
#define AUDIO_DEFAULT_THRESHOLD_MACHINE 3.0f   // escore z

/** Configuração inicial: 16 kHz/1024 no RP2040, 48 kHz/2048 no RP2350 (FPU). */
void  audio_config_defaults(Payload_Configuration *cfg);

/** Converte uma configuração validada para o motor, com a taxa efetiva da PIO. */
AcousticConfig audio_config_to_engine(const Payload_Configuration *cfg, float effective_rate_hz);

const char *audio_mode_name(uint8_t mode);
bool        audio_parse_mode(const char *text, uint8_t *mode);
const char *audio_window_name(uint8_t window);
bool        audio_parse_window(const char *text, uint8_t *window);

#endif /* AUDIO_CONFIG_H */
