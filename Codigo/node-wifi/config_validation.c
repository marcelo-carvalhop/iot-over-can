#include "config_validation.h"
#include <math.h>

bool edge_config_valid_mode(uint8_t mode) {
    return mode <= FSM_MODE_SEISMIC_STALTA;
}

bool edge_config_valid_sample_rate(float value) {
    return isfinite(value) && value >= EDGE_SAMPLE_RATE_MIN_HZ && value <= EDGE_SAMPLE_RATE_MAX_HZ;
}

bool edge_config_valid_window_size(uint16_t value) {
    return value == 128u || value == 256u || value == 512u;
}

bool edge_config_valid_window_type(uint8_t value) {
    return value <= 4u; /* WIN_BLACKMAN_H; kept protocol-layer independent */
}

bool edge_config_valid_stalta(float value) {
    return isfinite(value) && value > EDGE_STALTA_MIN_EXCLUSIVE && value <= EDGE_STALTA_MAX;
}

bool edge_config_valid_gain(float value) {
    return isfinite(value) && value > EDGE_CALIB_GAIN_MIN_EXCLUSIVE && value <= EDGE_CALIB_GAIN_MAX;
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
        case EDGE_CONFIG_ERR_WINDOW_SIZE: return "WINDOW_SIZE";
        case EDGE_CONFIG_ERR_WINDOW_TYPE: return "WINDOW_TYPE";
        case EDGE_CONFIG_ERR_STALTA: return "STALTA";
        case EDGE_CONFIG_ERR_GAIN: return "GAIN";
        default: return "UNKNOWN";
    }
}
