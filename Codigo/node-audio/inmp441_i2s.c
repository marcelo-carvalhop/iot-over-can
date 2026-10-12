/**
 * @file inmp441_i2s.c
 * @brief PIO (I2S mestre) + DMA em anel para o INMP441.
 *
 * Dois canais de DMA encadeados (A -> B -> A) copiam o FIFO de recepção da
 * PIO para um anel de INMP441_RING_BLOCKS blocos. Quando um canal termina, o
 * outro já está escrevendo o bloco seguinte; a interrupção apenas conta o
 * bloco completo e aponta o canal que terminou para o bloco depois desse.
 * Nenhuma amostra depende da latência do processamento.
 */
#include "inmp441_i2s.h"

#include <math.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"

#include "inmp441_i2s.pio.h"

static int32_t g_ring[INMP441_RING_BLOCKS][INMP441_BLOCK_FRAMES] __attribute__((aligned(4)));

static PIO   g_pio = NULL;
static uint  g_sm = 0;
static uint  g_offset = 0;
static int   g_dma[2] = {-1, -1};
static bool  g_ready = false;
static bool  g_running = false;
static bool  g_irq_installed = false;
static float g_rate_eff = 0.0f;
static Inmp441Channel g_channel = INMP441_CHANNEL_LEFT;

static volatile uint32_t g_completed = 0;   // blocos completos desde o início (escrito na IRQ)
static uint32_t g_consumed = 0;             // blocos já entregues e liberados

static void dma_irq_handler(void) {
    for (unsigned i = 0; i < 2u; ++i) {
        if (g_dma[i] < 0) continue;
        const uint ch = (uint)g_dma[i];
        if (!dma_irqn_get_channel_status(1, ch)) continue;
        dma_irqn_acknowledge_channel(1, ch);
        const uint32_t done = g_completed + 1u;
        g_completed = done;
        // O outro canal já escreve o bloco `done`; este fica pronto para o seguinte.
        dma_channel_set_write_addr(ch, g_ring[(done + 1u) % INMP441_RING_BLOCKS], false);
    }
}

bool inmp441_init(void) {
    if (g_ready) return true;
    const uint first = AUDIO_PIN_SCK < AUDIO_PIN_SD ? AUDIO_PIN_SCK : AUDIO_PIN_SD;
    const uint last = AUDIO_PIN_WS > AUDIO_PIN_SD ? AUDIO_PIN_WS : AUDIO_PIN_SD;
    // O rádio CYW43 também usa uma máquina de estados e dois canais de DMA,
    // reservados dinamicamente; aqui se faz o mesmo para não colidir com ele.
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(&inmp441_i2s_program, &g_pio, &g_sm, &g_offset, first,
                                                          last - first + 1u, true)) {
        return false;
    }
    g_dma[0] = dma_claim_unused_channel(false);
    g_dma[1] = dma_claim_unused_channel(false);
    if (g_dma[0] < 0 || g_dma[1] < 0) {
        if (g_dma[0] >= 0) dma_channel_unclaim((uint)g_dma[0]);
        if (g_dma[1] >= 0) dma_channel_unclaim((uint)g_dma[1]);
        g_dma[0] = g_dma[1] = -1;
        pio_remove_program_and_unclaim_sm(&inmp441_i2s_program, g_pio, g_sm, g_offset);
        return false;
    }
    g_ready = true;
    return true;
}

void inmp441_stop(void) {
    if (!g_ready || !g_running) return;
    pio_sm_set_enabled(g_pio, g_sm, false);
    for (unsigned i = 0; i < 2u; ++i) dma_irqn_set_channel_enabled(1, (uint)g_dma[i], false);
    // RP2040-E13: um canal encadeado pode ser disparado pelo outro durante o
    // abort. O encadeamento é desfeito (cada canal aponta para si) antes.
    for (unsigned i = 0; i < 2u; ++i) {
        dma_channel_config c = dma_get_channel_config((uint)g_dma[i]);
        channel_config_set_chain_to(&c, (uint)g_dma[i]);
        dma_channel_set_config((uint)g_dma[i], &c, false);
    }
    dma_channel_abort((uint)g_dma[0]);
    dma_channel_abort((uint)g_dma[1]);
    for (unsigned i = 0; i < 2u; ++i) dma_irqn_acknowledge_channel(1, (uint)g_dma[i]);
    pio_sm_clear_fifos(g_pio, g_sm);
    g_running = false;
}

float inmp441_start(float sample_rate_hz, Inmp441Channel channel) {
    if (!(sample_rate_hz > 0.0f) || !inmp441_init()) return 0.0f;
    inmp441_stop();

    // Divisor 16.8 da PIO: clk_sys / (128 · fs). A taxa efetiva é recalculada
    // a partir do divisor arredondado (125 MHz -> 16 kHz exatos, 48001,2 Hz).
    const double sys_hz = (double)clock_get_hz(clk_sys);
    const double div = sys_hz / (128.0 * (double)sample_rate_hz);
    uint32_t div_int = (uint32_t)div;
    uint32_t div_frac = (uint32_t)lround((div - (double)div_int) * 256.0);
    if (div_frac >= 256u) {
        div_int++;
        div_frac = 0;
    }
    if (div_int < 1u) {
        div_int = 1u;
        div_frac = 0;
    }
    g_rate_eff = (float)(sys_hz / (128.0 * ((double)div_int + (double)div_frac / 256.0)));

    inmp441_i2s_program_init(g_pio, g_sm, g_offset, AUDIO_PIN_SD, AUDIO_PIN_SCK, div_int, (uint8_t)div_frac);
    // Microfone com L/R em VDD: WS invertido no pino, a PIO segue lendo "WS = 0".
    gpio_set_outover(AUDIO_PIN_WS, channel == INMP441_CHANNEL_RIGHT ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
    g_channel = channel;

    g_completed = 0;
    g_consumed = 0;
    for (unsigned i = 0; i < 2u; ++i) {
        const uint ch = (uint)g_dma[i];
        dma_channel_config c = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_dreq(&c, pio_get_dreq(g_pio, g_sm, false));
        channel_config_set_chain_to(&c, (uint)g_dma[i ^ 1u]);
        dma_channel_configure(ch, &c, g_ring[i], &g_pio->rxf[g_sm], INMP441_BLOCK_FRAMES, false);
        dma_irqn_acknowledge_channel(1, ch);
        dma_irqn_set_channel_enabled(1, ch, true);
    }
    if (!g_irq_installed) {
        // Compartilhado: outro código pode usar DMA_IRQ_1 em outros canais.
        irq_add_shared_handler(DMA_IRQ_1, dma_irq_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        irq_set_enabled(DMA_IRQ_1, true);
        g_irq_installed = true;
    }
    dma_channel_start((uint)g_dma[0]);
    pio_sm_set_enabled(g_pio, g_sm, true);
    g_running = true;
    return g_rate_eff;
}

bool inmp441_is_running(void) { return g_running; }
float inmp441_effective_rate_hz(void) { return g_rate_eff; }
Inmp441Channel inmp441_channel(void) { return g_channel; }

bool inmp441_acquire(const int32_t **block, uint32_t *lost_blocks) {
    if (lost_blocks) *lost_blocks = 0;
    if (!block || !g_running) return false;
    const uint32_t completed = g_completed;
    if (g_consumed == completed) return false;
    // Os dois blocos à frente estão com a DMA; o resto do anel é seguro.
    const uint32_t safe = INMP441_RING_BLOCKS - 2u;
    if (completed - g_consumed > safe) {
        if (lost_blocks) *lost_blocks = completed - g_consumed - safe;
        g_consumed = completed - safe;
    }
    *block = g_ring[g_consumed % INMP441_RING_BLOCKS];
    return true;
}

void inmp441_release(void) {
    if (g_consumed != g_completed) g_consumed++;
}

uint32_t inmp441_backlog(void) {
    return g_running ? g_completed - g_consumed : 0u;
}

void inmp441_flush(void) {
    g_consumed = g_completed;
}

uint32_t inmp441_startup_ms(float sample_rate_hz) {
    if (!(sample_rate_hz > 0.0f)) return 0;
    // 2^18 ciclos de SCK (SCK = 64 · fs) com a saída em zero, mais margem.
    return (uint32_t)(262144.0f * 1000.0f / (64.0f * sample_rate_hz)) + 50u;
}
