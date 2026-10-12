/**
 * @file audio_config.c
 * @brief Validação de Payload_Configuration no perfil acústico (API de config_validation.h).
 */
#include "audio_config.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

const uint32_t AUDIO_SAMPLE_RATES_HZ[AUDIO_RATE_COUNT] = {8000u, 16000u, 24000u, 32000u, 48000u};
const uint16_t AUDIO_FFT_SIZES[AUDIO_FFT_SIZE_COUNT] = {256u, 512u, 1024u, 2048u};

bool edge_config_valid_mode(uint8_t mode) {
    return mode <= ACOUSTIC_MODE_EVENTS;
}

bool edge_config_valid_sample_rate(float value) {
    if (!isfinite(value)) return false;
    for (unsigned i = 0; i < AUDIO_RATE_COUNT; ++i) {
        if (fabsf(value - (float)AUDIO_SAMPLE_RATES_HZ[i]) <= 0.5f) return true;
    }
    return false;
}

bool edge_config_valid_window_size(uint16_t value) {
    for (unsigned i = 0; i < AUDIO_FFT_SIZE_COUNT; ++i) {
        if (value == AUDIO_FFT_SIZES[i]) return true;
    }
    return false;
}

bool edge_config_valid_window_type(uint8_t value) {
    return value <= 4u;
}

bool edge_config_valid_stalta(float value) {
    return isfinite(value) && value > AUDIO_THRESHOLD_MIN_EXCLUSIVE && value <= AUDIO_THRESHOLD_MAX;
}

bool edge_config_valid_gain(float value) {
    return isfinite(value) && value > AUDIO_GAIN_MIN_EXCLUSIVE && value <= AUDIO_GAIN_MAX;
}

bool edge_config_validate(const Payload_Configuration *cfg, EdgeConfigValidationError *error) {
    EdgeConfigValidationError local = EDGE_CONFIG_OK;
    if (!cfg) local = EDGE_CONFIG_ERR_NULL;
    else if (!edge_config_valid_mode(cfg->target_mode)) local = EDGE_CONFIG_ERR_MODE;
    else if (!edge_config_valid_sample_rate(cfg->sample_rate_hz)) local = EDGE_CONFIG_ERR_SAMPLE_RATE;
    else if (!edge_config_valid_window_size(cfg->window_size)) local = EDGE_CONFIG_ERR_WINDOW_SIZE;
    else if (!edge_config_valid_window_type(cfg->window_type)) local = EDGE_CONFIG_ERR_WINDOW_TYPE;
    else if (!edge_config_valid_stalta(cfg->stalta_thresh)) local = EDGE_CONFIG_ERR_STALTA;
    else if (!edge_config_valid_gain(cfg->calib_gain)) local = EDGE_CONFIG_ERR_GAIN;
    if (error) *error = local;
    return local == EDGE_CONFIG_OK;
}

const char *edge_config_validation_error_name(EdgeConfigValidationError error) {
    switch (error) {
        case EDGE_CONFIG_OK: return "OK";
        case EDGE_CONFIG_ERR_NULL: return "NULL";
        case EDGE_CONFIG_ERR_MODE: return "MODE";
        case EDGE_CONFIG_ERR_SAMPLE_RATE: return "SAMPLE_RATE";
        case EDGE_CONFIG_ERR_WINDOW_SIZE: return "FFT_SIZE";
        case EDGE_CONFIG_ERR_WINDOW_TYPE: return "WINDOW_TYPE";
        case EDGE_CONFIG_ERR_STALTA: return "THRESHOLD";
        case EDGE_CONFIG_ERR_GAIN: return "GAIN";
        default: return "UNKNOWN";
    }
}

void audio_config_defaults(Payload_Configuration *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic_header = NET_MAGIC_HEADER;
    cfg->cmd_type = CMD_SET_CONFIG;
    cfg->target_mode = ACOUSTIC_MODE_LEVEL;
#if defined(PICO_RP2350) && PICO_RP2350
    // O Cortex-M33 com FPU acompanha a banda inteira do INMP441 (até 15 kHz).
    cfg->sample_rate_hz = 48000.0f;
    cfg->window_size = 2048;
#else
    // O Cortex-M0+ emula ponto flutuante: 16 kHz cobre até 8 kHz com folga de CPU.
    cfg->sample_rate_hz = 16000.0f;
    cfg->window_size = 1024;
#endif
    cfg->window_type = 1;   // Hann
    cfg->stalta_thresh = AUDIO_DEFAULT_THRESHOLD_EVENTS;
    cfg->calib_gain = 1.0f;
}

AcousticConfig audio_config_to_engine(const Payload_Configuration *cfg, float effective_rate_hz) {
    AcousticConfig out;
    out.mode = cfg->target_mode;
    out.sample_rate_hz = effective_rate_hz > 0.0f ? effective_rate_hz : cfg->sample_rate_hz;
    out.fft_size = cfg->window_size;
    out.window_type = cfg->window_type;
    out.threshold = cfg->stalta_thresh;
    out.gain = cfg->calib_gain;
    return out;
}

static bool text_ieq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

const char *audio_mode_name(uint8_t mode) {
    switch (mode) {
        case ACOUSTIC_MODE_IDLE:    return "IDLE";
        case ACOUSTIC_MODE_LEVEL:   return "LEVEL";
        case ACOUSTIC_MODE_MACHINE: return "MACHINE";
        case ACOUSTIC_MODE_EVENTS:  return "EVENTS";
        default:                    return "UNKNOWN";
    }
}

bool audio_parse_mode(const char *text, uint8_t *mode) {
    static const char *const names[] = {"IDLE", "LEVEL", "MACHINE", "EVENTS"};
    if (!text || !mode) return false;
    for (uint8_t i = 0; i < 4u; ++i) {
        if (text_ieq(text, names[i])) {
            *mode = i;
            return true;
        }
    }
    char *end = NULL;
    const long v = strtol(text, &end, 10);
    if (end != text && *end == '\0' && v >= 0 && v <= ACOUSTIC_MODE_EVENTS) {
        *mode = (uint8_t)v;
        return true;
    }
    return false;
}

const char *audio_window_name(uint8_t window) {
    switch (window) {
        case 0: return "RECT";
        case 1: return "HANN";
        case 2: return "HAMMING";
        case 3: return "FLATTOP";
        case 4: return "BLACKMAN";
        default: return "UNKNOWN";
    }
}

bool audio_parse_window(const char *text, uint8_t *window) {
    static const char *const names[] = {"RECT", "HANN", "HAMMING", "FLATTOP", "BLACKMAN"};
    if (!text || !window) return false;
    if (text_ieq(text, "RECTANGULAR")) {
        *window = 0;
        return true;
    }
    for (uint8_t i = 0; i < 5u; ++i) {
        if (text_ieq(text, names[i])) {
            *window = i;
            return true;
        }
    }
    char *end = NULL;
    const long v = strtol(text, &end, 10);
    if (end != text && *end == '\0' && v >= 0 && v <= 4) {
        *window = (uint8_t)v;
        return true;
    }
    return false;
}
