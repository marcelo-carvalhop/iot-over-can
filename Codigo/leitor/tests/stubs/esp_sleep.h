/* Sono de mentira: o teste escolhe se um botão acorda o ESP32 ou se o tempo
 * passa até o despertador (o relógio avança o tempo do sono). */
#pragma once

#include <cstdint>

#include "Arduino.h"
#include "M5Unified.h"

typedef enum { ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_TIMER, ESP_SLEEP_WAKEUP_GPIO } esp_sleep_wakeup_cause_t;
typedef esp_sleep_wakeup_cause_t esp_sleep_source_t;

namespace stub {
struct Desligou {};
inline bool botaoAcorda = true;
inline uint64_t despertadorUs = 0;
inline int sonos = 0;
inline esp_sleep_wakeup_cause_t causa = ESP_SLEEP_WAKEUP_UNDEFINED;
inline uint8_t teclaAoAcordar = 0;  // chega no primeiro despertar com o CardKB
}  // namespace stub

inline int esp_sleep_enable_gpio_wakeup() { return 0; }
inline int esp_sleep_enable_timer_wakeup(uint64_t us) {
  stub::despertadorUs = us;
  return 0;
}
inline int esp_sleep_disable_wakeup_source(esp_sleep_source_t) { return 0; }
inline int esp_light_sleep_start() {
  ++stub::sonos;
  if (stub::botaoAcorda) {
    stub::causa = ESP_SLEEP_WAKEUP_GPIO;
    stub::agoraMs += 1000;
  } else {
    stub::causa = ESP_SLEEP_WAKEUP_TIMER;
    stub::agoraMs += static_cast<uint32_t>(stub::despertadorUs / 1000);
    if (stub::teclaAoAcordar) stub::tecla = stub::teclaAoAcordar;
    stub::teclaAoAcordar = 0;
  }
  return 0;
}
inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return stub::causa; }
[[noreturn]] inline void esp_deep_sleep_start() { throw stub::Desligou{}; }
