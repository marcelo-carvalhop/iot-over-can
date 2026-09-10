#ifndef CONFIG_VALIDATION_H
#define CONFIG_VALIDATION_H

#include <stdbool.h>
#include <stdint.h>
#include "edge_protocol_definitions.h"

#define EDGE_SAMPLE_RATE_MIN_HZ 4.0f
#define EDGE_SAMPLE_RATE_MAX_HZ 1000.0f
#define EDGE_STALTA_MIN_EXCLUSIVE 1.0f
#define EDGE_STALTA_MAX 1000.0f
#define EDGE_CALIB_GAIN_MIN_EXCLUSIVE 0.0f
#define EDGE_CALIB_GAIN_MAX 1000.0f

typedef enum {
    EDGE_CONFIG_OK = 0,
    EDGE_CONFIG_ERR_NULL,
    EDGE_CONFIG_ERR_MODE,
    EDGE_CONFIG_ERR_SAMPLE_RATE,
    EDGE_CONFIG_ERR_WINDOW_SIZE,
    EDGE_CONFIG_ERR_WINDOW_TYPE,
    EDGE_CONFIG_ERR_STALTA,
    EDGE_CONFIG_ERR_GAIN
} EdgeConfigValidationError;

bool edge_config_valid_mode(uint8_t mode);
bool edge_config_valid_sample_rate(float value);
bool edge_config_valid_window_size(uint16_t value);
bool edge_config_valid_window_type(uint8_t value);
bool edge_config_valid_stalta(float value);
bool edge_config_valid_gain(float value);
bool edge_config_validate(const Payload_Configuration *cfg, EdgeConfigValidationError *error);
const char *edge_config_validation_error_name(EdgeConfigValidationError error);

#endif
