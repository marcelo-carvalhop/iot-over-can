#ifndef HOST_ESP_SYSTEM_H
#define HOST_ESP_SYSTEM_H

// Motivo do último reinício (esp_system.h do ESP-IDF). Na bancada virtual toda
// partida é "energia ligada", salvo quando um cenário escolhe outro motivo
// (sim_reset_reason, em sim_api.h); a classificação dos motivos está testada
// em Codigo/common/tests/test_ioc_resetlog.c.
typedef enum {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON = 1,
  ESP_RST_EXT = 2,
  ESP_RST_SW = 3,
  ESP_RST_PANIC = 4,
  ESP_RST_INT_WDT = 5,
  ESP_RST_TASK_WDT = 6,
  ESP_RST_WDT = 7,
  ESP_RST_DEEPSLEEP = 8,
  ESP_RST_BROWNOUT = 9,
  ESP_RST_SDIO = 10,
} esp_reset_reason_t;

esp_reset_reason_t esp_reset_reason(void);

#endif
