/**
 * @file audio_core1.c
 * @brief Laço do núcleo 1: blocos de DMA -> acoustic_dsp.c -> fila de relatórios.
 */
#include "audio_core1.h"

#include <math.h>
#include <string.h>

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"

#include "audio_config.h"
#include "inmp441_i2s.h"

#define CMD_QUEUE_LEN     4
#define RESULT_QUEUE_LEN  4
#define REPORT_QUEUE_LEN  2
#define CORE1_STACK_WORDS 2048    // 8 KiB

static AcousticEngine g_engine;               // ~70 KiB: só o núcleo 1 o toca
static AudioReportMessage g_msg;              // montado no núcleo 1, copiado para a fila
static uint32_t g_core1_stack[CORE1_STACK_WORDS];

static queue_t g_cmd_queue;
static queue_t g_result_queue;
static queue_t g_report_queue;

static Payload_Configuration g_initial;       // escrita antes do lançamento do núcleo 1
static Payload_Configuration g_cfg;           // configuração em vigor (núcleo 1)

static volatile uint32_t g_heartbeat = 0;
static volatile float    g_rate_eff = 0.0f;
static volatile uint8_t  g_channel = INMP441_CHANNEL_LEFT;
static volatile bool     g_hw_ok = false;

// Estado próprio do núcleo 1.
static uint64_t g_startup_until_us = 0;      // microfone emudecido após (re)início
static uint64_t g_window_start_us = 0;       // janela da medição de carga
static uint64_t g_busy_us = 0;
static uint32_t g_lost_total = 0;
static uint32_t g_dropped_reports = 0;
static bool     g_channel_locked = false;    // canal com sinal confirmado
static uint8_t  g_channel_attempts = 0;
static bool     g_channel_switched = false;

static void start_acquisition(Inmp441Channel channel) {
    const float fs = inmp441_start(g_cfg.sample_rate_hz, channel);
    g_rate_eff = fs;
    g_channel = (uint8_t)channel;
    g_startup_until_us = time_us_64() + (uint64_t)inmp441_startup_ms(fs) * 1000u;
}

static bool apply_config(const Payload_Configuration *cfg) {
    const Payload_Configuration previous = g_cfg;
    g_cfg = *cfg;
    if (cfg->target_mode == ACOUSTIC_MODE_IDLE) {
        // Ocioso: microfone sem relógio (o INMP441 entra em repouso sem SCK).
        inmp441_stop();
        AcousticConfig idle = audio_config_to_engine(cfg, cfg->sample_rate_hz);
        return acoustic_engine_configure(&g_engine, &idle);
    }
    const bool restart = !inmp441_is_running() || fabsf(cfg->sample_rate_hz - previous.sample_rate_hz) > 0.5f;
    if (restart) {
        start_acquisition((Inmp441Channel)g_channel);
        if (!(g_rate_eff > 0.0f)) {
            g_cfg = previous;
            return false;
        }
    }
    AcousticConfig engine_cfg = audio_config_to_engine(cfg, g_rate_eff);
    if (!acoustic_engine_configure(&g_engine, &engine_cfg)) {
        g_cfg = previous;
        return false;
    }
    // Recalcular janelas e ponderações leva alguns décimos de segundo no
    // RP2040; o áudio acumulado nesse meio-tempo pertence à medição anterior.
    inmp441_flush();
    return true;
}

// Troca automática de canal: um microfone com L/R em VDD só responde com WS
// em 1. Se o canal em uso fica em silêncio digital por 1 s, tenta o outro;
// depois de uma tentativa em cada lado, permanece no esquerdo e o relatório
// segue indicando a falha (DTC 0x1003 no núcleo 0). Uma nova configuração
// recomeça a busca.
static void check_channel(void) {
    if (g_channel_locked || g_cfg.target_mode == ACOUSTIC_MODE_IDLE) return;
    const uint8_t mic = acoustic_engine_mic_state(&g_engine);
    if (mic != ACOUSTIC_MIC_SILENT) return;
    if (g_channel_attempts >= 2u) {
        if (g_channel != INMP441_CHANNEL_LEFT) start_acquisition(INMP441_CHANNEL_LEFT);
        g_channel_locked = true;
        return;
    }
    g_channel_attempts++;
    const Inmp441Channel next = g_channel == INMP441_CHANNEL_LEFT ? INMP441_CHANNEL_RIGHT : INMP441_CHANNEL_LEFT;
    start_acquisition(next);
    AcousticConfig engine_cfg = audio_config_to_engine(&g_cfg, g_rate_eff);
    (void)acoustic_engine_configure(&g_engine, &engine_cfg);   // zera a contagem de silêncio
    g_channel_switched = true;
}

static AcousticFftPolicy policy_for_backlog(uint32_t backlog) {
    if (backlog > (INMP441_RING_BLOCKS * 3u) / 4u) return ACOUSTIC_FFT_NONE;
    if (backlog > INMP441_RING_BLOCKS / 2u) return ACOUSTIC_FFT_LOADED;
    return ACOUSTIC_FFT_NORMAL;
}

static void publish_report(void) {
    const uint64_t now = time_us_64();
    const uint64_t window = now - g_window_start_us;
    g_msg.cpu_pct = (uint8_t)(window > 0 ? (g_busy_us * 100u + window / 2u) / window : 0u);
    if (g_msg.cpu_pct > 100u) g_msg.cpu_pct = 100u;
    g_window_start_us = now;
    g_busy_us = 0;
    g_msg.channel = g_channel;
    g_msg.channel_switched = g_channel_switched;
    g_channel_switched = false;
    g_msg.lost_blocks_total = g_lost_total;
    g_msg.dropped_reports = g_dropped_reports;
    if (g_msg.report.mic_state == ACOUSTIC_MIC_OK && !g_channel_locked) {
        g_channel_locked = true;   // um segundo inteiro com sinal: canal confirmado
    }
    if (!queue_try_add(&g_report_queue, &g_msg)) g_dropped_reports++;
}

static bool process_blocks(void) {
    bool any = false;
    const int32_t *block = NULL;
    uint32_t lost = 0;
    while (inmp441_acquire(&block, &lost)) {
        any = true;
        if (lost) {
            g_lost_total += lost;
            acoustic_engine_note_gap(&g_engine, lost * INMP441_BLOCK_FRAMES);
        }
        const uint64_t t0 = time_us_64();
        if (t0 < g_startup_until_us) {
            inmp441_release();   // saída do INMP441 ainda em zero após a partida
            continue;
        }
        acoustic_engine_process(&g_engine, block, INMP441_BLOCK_FRAMES, policy_for_backlog(inmp441_backlog()));
        inmp441_release();
        g_busy_us += time_us_64() - t0;
        if (acoustic_engine_take_report(&g_engine, &g_msg.report)) publish_report();
        check_channel();
    }
    return any;
}

static void core1_main(void) {
    acoustic_engine_init(&g_engine);
    g_hw_ok = inmp441_init();
    memset(&g_cfg, 0, sizeof(g_cfg));
    if (g_hw_ok) (void)apply_config(&g_initial);
    g_window_start_us = time_us_64();

    for (;;) {
        g_heartbeat++;
        AudioCommand cmd;
        while (queue_try_remove(&g_cmd_queue, &cmd)) {
            AudioCommandResult res;
            memset(&res, 0, sizeof(res));
            res.type = cmd.type;
            res.origin = cmd.origin;
            res.token = cmd.token;
            res.action = cmd.action;
            res.config = cmd.config;
            if (cmd.type == AUDIO_CMD_CONFIG) {
                res.ok = g_hw_ok && apply_config(&cmd.config);
                if (res.ok) {
                    g_channel_locked = false;   // nova configuração: confere o canal de novo
                    g_channel_attempts = 0;
                }
            } else {
                res.ok = acoustic_engine_control(&g_engine, cmd.action, cmd.argument);
            }
            res.effective_rate_hz = g_rate_eff;
            (void)queue_try_add(&g_result_queue, &res);
        }
        if (!process_blocks()) busy_wait_us_32(200);
    }
}

bool audio_core1_start(const Payload_Configuration *initial) {
    if (!initial) return false;
    g_initial = *initial;
    g_rate_eff = initial->sample_rate_hz;
    queue_init(&g_cmd_queue, sizeof(AudioCommand), CMD_QUEUE_LEN);
    queue_init(&g_result_queue, sizeof(AudioCommandResult), RESULT_QUEUE_LEN);
    queue_init(&g_report_queue, sizeof(AudioReportMessage), REPORT_QUEUE_LEN);
    multicore_launch_core1_with_stack(core1_main, g_core1_stack, sizeof(g_core1_stack));
    return true;
}

bool audio_core1_post(const AudioCommand *cmd) {
    return cmd && queue_try_add(&g_cmd_queue, cmd);
}

bool audio_core1_poll_result(AudioCommandResult *out) {
    return out && queue_try_remove(&g_result_queue, out);
}

bool audio_core1_poll_report(AudioReportMessage *out) {
    return out && queue_try_remove(&g_report_queue, out);
}

uint32_t audio_core1_heartbeat(void) { return g_heartbeat; }
float    audio_core1_effective_rate_hz(void) { return g_rate_eff; }
uint8_t  audio_core1_channel(void) { return g_channel; }
bool     audio_core1_hardware_ok(void) { return g_hw_ok; }
