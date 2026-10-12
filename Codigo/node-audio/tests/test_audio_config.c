/*
 * Validação de Payload_Configuration no perfil acústico (audio_config.c).
 * Mesmas funções que o driver de rede compartilhado chama para o sensor de
 * vibração, com as faixas do INMP441.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio_config.h"

static Payload_Configuration valid_config(void) {
    Payload_Configuration cfg;
    audio_config_defaults(&cfg);
    cfg.request_counter = 1;
    return cfg;
}

int main(void) {
    EdgeConfigValidationError err = EDGE_CONFIG_OK;
    Payload_Configuration cfg = valid_config();
    // Sem PICO_RP2350 (computador ou RP2040): 16 kHz, FFT de 1024 pontos, modo LEVEL.
    assert(cfg.sample_rate_hz == 16000.0f && cfg.window_size == 1024 && cfg.target_mode == ACOUSTIC_MODE_LEVEL);
    assert(edge_config_validate(&cfg, &err) && err == EDGE_CONFIG_OK);

    const float rates[] = {8000.0f, 16000.0f, 24000.0f, 32000.0f, 48000.0f};
    for (unsigned i = 0; i < 5; ++i) {
        cfg = valid_config();
        cfg.sample_rate_hz = rates[i];
        assert(edge_config_validate(&cfg, &err));
    }
    const float bad_rates[] = {1000.0f, 44100.0f, 16001.0f, 96000.0f, NAN};
    for (unsigned i = 0; i < 5; ++i) {
        cfg = valid_config();
        cfg.sample_rate_hz = bad_rates[i];
        assert(!edge_config_validate(&cfg, &err) && err == EDGE_CONFIG_ERR_SAMPLE_RATE);
    }

    cfg = valid_config();
    cfg.window_size = 128;
    assert(!edge_config_validate(&cfg, &err) && err == EDGE_CONFIG_ERR_WINDOW_SIZE);
    cfg.window_size = 2048;
    assert(edge_config_validate(&cfg, &err));

    cfg = valid_config();
    cfg.target_mode = 4;
    assert(!edge_config_validate(&cfg, &err) && err == EDGE_CONFIG_ERR_MODE);

    cfg = valid_config();
    cfg.stalta_thresh = 1.0f;
    assert(!edge_config_validate(&cfg, &err) && err == EDGE_CONFIG_ERR_STALTA);

    cfg = valid_config();
    cfg.calib_gain = 0.0f;
    assert(!edge_config_validate(&cfg, &err) && err == EDGE_CONFIG_ERR_GAIN);
    cfg.calib_gain = 150.0f;
    assert(!edge_config_validate(&cfg, &err));
    assert(strcmp(edge_config_validation_error_name(EDGE_CONFIG_ERR_WINDOW_SIZE), "FFT_SIZE") == 0);

    uint8_t value = 0;
    assert(audio_parse_mode("machine", &value) && value == ACOUSTIC_MODE_MACHINE);
    assert(audio_parse_mode("3", &value) && value == ACOUSTIC_MODE_EVENTS);
    assert(!audio_parse_mode("ROTATING", &value));
    assert(audio_parse_window("Blackman", &value) && value == 4);
    assert(strcmp(audio_mode_name(ACOUSTIC_MODE_LEVEL), "LEVEL") == 0);

    AcousticConfig engine = audio_config_to_engine(&cfg, 15999.5f);
    assert(engine.sample_rate_hz == 15999.5f && engine.fft_size == cfg.window_size);

    printf("audio_config native tests: PASS\n");
    return 0;
}
