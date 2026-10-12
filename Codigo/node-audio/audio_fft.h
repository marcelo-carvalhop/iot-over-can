/**
 * @file audio_fft.h
 * @brief FFT de sinal real (N até 2048) por meio de uma FFT complexa de N/2 pontos.
 *
 * Código C puro, sem dependência do Pico SDK: compila no computador para os
 * testes (Codigo/node-audio/tests) e no RP2040/RP2350 para o firmware.
 *
 * O vetor real de N amostras é lido como N/2 números complexos intercalados
 * z[m] = x[2m] + j·x[2m+1] e transformado no próprio lugar. A separação final
 * (Xe ± W·Xo) produz diretamente |X[k]|² para k = 0..N/2, que é tudo o que o
 * processamento acústico usa.
 */
#ifndef AUDIO_FFT_H
#define AUDIO_FFT_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_FFT_MIN_N 256
#define AUDIO_FFT_MAX_N 2048

typedef struct {
    uint16_t n;                                  // pontos reais (potência de 2)
    float    cos_tab[AUDIO_FFT_MAX_N / 2];       // cos(2πk/N), k < N/2
    float    sin_tab[AUDIO_FFT_MAX_N / 2];       // sin(2πk/N)
    uint16_t bitrev[AUDIO_FFT_MAX_N / 2];        // permutação de N/2 pontos
} AudioFft;

/** Prepara tabelas para N pontos reais; false se N não é potência de 2 em [256, 2048]. */
bool audio_fft_init(AudioFft *fft, uint16_t n);

/**
 * Espectro de potência de N amostras reais.
 *   data   N amostras (destruídas: viram o resultado complexo intermediário)
 *   power  N/2 + 1 valores: power[k] = |X[k]|², k = 0 (DC) .. N/2 (Nyquist)
 */
void audio_fft_power(const AudioFft *fft, float *data, float *power);

#endif /* AUDIO_FFT_H */
