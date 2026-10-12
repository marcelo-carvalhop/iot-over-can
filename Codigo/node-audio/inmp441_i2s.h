/**
 * @file inmp441_i2s.h
 * @brief Aquisição contínua do INMP441: PIO (I2S mestre) + DMA em anel.
 *
 * Usado apenas pelo núcleo 1. A interrupção de DMA é registrada no núcleo
 * que chama inmp441_start(), e é ela que rearma os canais: a aquisição nunca
 * para, mesmo que o processamento atrase. O consumidor lê os blocos em ordem
 * com inmp441_acquire()/inmp441_release(); se ficar mais de
 * INMP441_RING_BLOCKS - 2 blocos para trás, os mais antigos são descartados e
 * contados como perdidos.
 *
 * Ligação padrão (Pico W e Pico 2 W, pinos vizinhos no conector):
 *
 *   INMP441   Pico            pino físico
 *   SCK       GP18            24
 *   WS        GP19            25   (sempre SCK + 1: exigência do side-set)
 *   SD        GP20            26
 *   L/R       GND             23   (canal esquerdo; em VDD, canal direito)
 *   VDD       3V3(OUT)        36
 *   GND       GND             23/28
 *
 * Os pinos podem ser trocados no build (-DAUDIO_PIN_SCK=.. -DAUDIO_PIN_SD=..).
 */
#ifndef INMP441_I2S_H
#define INMP441_I2S_H

#include <stdbool.h>
#include <stdint.h>

#ifndef AUDIO_PIN_SCK
#define AUDIO_PIN_SCK 18
#endif
#define AUDIO_PIN_WS (AUDIO_PIN_SCK + 1)
#ifndef AUDIO_PIN_SD
#define AUDIO_PIN_SD 20
#endif

#define INMP441_BLOCK_FRAMES  256   // palavras por bloco de DMA (uma por quadro estéreo)
#define INMP441_RING_BLOCKS   16    // anel de 16 KiB: ~256 ms a 16 kHz, ~85 ms a 48 kHz

typedef enum {
    INMP441_CHANNEL_LEFT  = 0,      // L/R em GND: dados com WS = 0
    INMP441_CHANNEL_RIGHT = 1       // L/R em VDD: dados com WS = 1
} Inmp441Channel;

/** Reserva PIO, máquina de estados e dois canais de DMA. false se não houver recursos. */
bool     inmp441_init(void);

/**
 * Inicia (ou reinicia) a aquisição. Retorna a taxa efetiva, que pode diferir
 * da pedida pelo arredondamento do divisor 16.8 da PIO (ex.: 48001,2 Hz no
 * RP2040 a 125 MHz). Deve ser chamada pelo núcleo que tratará a interrupção.
 */
float    inmp441_start(float sample_rate_hz, Inmp441Channel channel);
void     inmp441_stop(void);
bool     inmp441_is_running(void);
float    inmp441_effective_rate_hz(void);
Inmp441Channel inmp441_channel(void);

/**
 * Próximo bloco completo, em ordem. `lost_blocks` recebe quantos blocos foram
 * sobrescritos desde a última chamada (0 normalmente).
 */
bool     inmp441_acquire(const int32_t **block, uint32_t *lost_blocks);
void     inmp441_release(void);
/** Blocos completos aguardando processamento. */
uint32_t inmp441_backlog(void);
/** Descarta os blocos acumulados sem contá-los como perdidos (após reconfigurar o DSP). */
void     inmp441_flush(void);
/** Tempo desde o início em que o microfone ainda está emudecido (2^18 ciclos de SCK). */
uint32_t inmp441_startup_ms(float sample_rate_hz);

#endif /* INMP441_I2S_H */
