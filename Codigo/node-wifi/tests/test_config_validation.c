#include <assert.h>
#include <math.h>
#include <string.h>
#include "config_validation.h"

static Payload_Configuration valid_config(void) {
    Payload_Configuration cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic_header = NET_MAGIC_HEADER;
    cfg.cmd_type = CMD_SET_CONFIG;
    cfg.session_token = 1;
    cfg.request_counter = 1;
    cfg.target_mode = FSM_MODE_ROTATING_MACH;
    cfg.sample_rate_hz = 1000.0f;
    cfg.window_size = 512;
    cfg.window_type = 1;
    cfg.stalta_thresh = 4.0f;
    cfg.calib_gain = 1.0f;
    return cfg;
}

int main(void) {
    EdgeConfigValidationError err = EDGE_CONFIG_OK;
    Payload_Configuration cfg = valid_config();
    assert(edge_config_validate(&cfg, &err));
    assert(err == EDGE_CONFIG_OK);

    cfg.sample_rate_hz = 1000.1f;
    assert(!edge_config_validate(&cfg, &err));
    assert(err == EDGE_CONFIG_ERR_SAMPLE_RATE);

    cfg = valid_config();
    cfg.sample_rate_hz = NAN;
    assert(!edge_config_validate(&cfg, &err));
    assert(err == EDGE_CONFIG_ERR_SAMPLE_RATE);

    cfg = valid_config();
    cfg.sample_rate_hz = INFINITY;
    assert(!edge_config_validate(&cfg, &err));
    assert(err == EDGE_CONFIG_ERR_SAMPLE_RATE);

    cfg = valid_config();
    cfg.window_size = 255;
    assert(!edge_config_validate(&cfg, &err));
    assert(err == EDGE_CONFIG_ERR_WINDOW_SIZE);

    cfg = valid_config();
    cfg.stalta_thresh = 1.0f;
    assert(!edge_config_validate(&cfg, &err));
    assert(err == EDGE_CONFIG_ERR_STALTA);

    cfg = valid_config();
    cfg.calib_gain = -1.0f;
    assert(!edge_config_validate(&cfg, &err));
    assert(err == EDGE_CONFIG_ERR_GAIN);

    return 0;
}
