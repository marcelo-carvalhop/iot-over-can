/**
 * @file audio_fft.c
 * @brief FFT real por FFT complexa de N/2 pontos (radix-2, decimação no tempo).
 */
#include "audio_fft.h"

#include <math.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static unsigned log2_u16(uint16_t v) {
    unsigned r = 0;
    while ((1u << r) < v) r++;
    return r;
}

bool audio_fft_init(AudioFft *fft, uint16_t n) {
    if (!fft || n < AUDIO_FFT_MIN_N || n > AUDIO_FFT_MAX_N || (n & (n - 1u)) != 0) return false;
    fft->n = n;
    const uint16_t half = (uint16_t)(n / 2u);
    // Tabelas calculadas em dupla precisão uma única vez, na configuração.
    for (uint16_t k = 0; k < half; ++k) {
        const double angle = 2.0 * M_PI * (double)k / (double)n;
        fft->cos_tab[k] = (float)cos(angle);
        fft->sin_tab[k] = (float)sin(angle);
    }
    const unsigned bits = log2_u16(half);
    for (uint16_t i = 0; i < half; ++i) {
        uint16_t r = 0;
        for (unsigned b = 0; b < bits; ++b) {
            if (i & (1u << b)) r = (uint16_t)(r | (1u << (bits - 1u - b)));
        }
        fft->bitrev[i] = r;
    }
    return true;
}

// FFT complexa de m = N/2 pontos, no lugar, com valores intercalados (re, im).
static void fft_complex(const AudioFft *fft, float *z) {
    const uint16_t n = fft->n;
    const uint16_t m = (uint16_t)(n / 2u);

    for (uint16_t i = 0; i < m; ++i) {
        const uint16_t j = fft->bitrev[i];
        if (i < j) {
            float t = z[2u * i];
            z[2u * i] = z[2u * j];
            z[2u * j] = t;
            t = z[2u * i + 1u];
            z[2u * i + 1u] = z[2u * j + 1u];
            z[2u * j + 1u] = t;
        }
    }

    for (uint16_t size = 2; size <= m; size = (uint16_t)(size << 1)) {
        const uint16_t half = (uint16_t)(size >> 1);
        // W_size^j = W_N^(j·N/size): a mesma tabela de N pontos serve a todos os estágios.
        const uint16_t stride = (uint16_t)(n / size);
        for (uint16_t j = 0; j < half; ++j) {
            const float wr = fft->cos_tab[j * stride];
            const float wi = -fft->sin_tab[j * stride];
            for (uint16_t a = j; a < m; a = (uint16_t)(a + size)) {
                const uint16_t b = (uint16_t)(a + half);
                float *za = &z[2u * a];
                float *zb = &z[2u * b];
                const float tr = wr * zb[0] - wi * zb[1];
                const float ti = wr * zb[1] + wi * zb[0];
                zb[0] = za[0] - tr;
                zb[1] = za[1] - ti;
                za[0] += tr;
                za[1] += ti;
            }
        }
    }
}

void audio_fft_power(const AudioFft *fft, float *data, float *power) {
    if (!fft || !data || !power || fft->n == 0) return;
    const uint16_t n = fft->n;
    const uint16_t m = (uint16_t)(n / 2u);

    fft_complex(fft, data);

    // X[0] e X[N/2] são reais: Re(Z0) ± Im(Z0).
    const float z0r = data[0];
    const float z0i = data[1];
    power[0] = (z0r + z0i) * (z0r + z0i);
    power[m] = (z0r - z0i) * (z0r - z0i);

    // Para k = 1..m-1, com a = Z[k] e b = conj(Z[m-k]):
    //   Xe = (a + b)/2,  Xo = (a - b)/(2j),  X[k] = Xe + W_N^k·Xo.
    for (uint16_t k = 1; k < m; ++k) {
        const float ar = data[2u * k];
        const float ai = data[2u * k + 1u];
        const float br = data[2u * (m - k)];
        const float bi = -data[2u * (m - k) + 1u];
        const float xer = 0.5f * (ar + br);
        const float xei = 0.5f * (ai + bi);
        // (a - b)/(2j) = -j·(a - b)/2
        const float dr = ar - br;
        const float di = ai - bi;
        const float xor_ = 0.5f * di;
        const float xoi = -0.5f * dr;
        const float wr = fft->cos_tab[k];
        const float wi = -fft->sin_tab[k];
        const float xr = xer + (wr * xor_ - wi * xoi);
        const float xi = xei + (wr * xoi + wi * xor_);
        power[k] = xr * xr + xi * xi;
    }
}
