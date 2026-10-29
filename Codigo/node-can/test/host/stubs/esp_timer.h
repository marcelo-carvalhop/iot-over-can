// Relógio monotônico do ESP-IDF na bancada virtual: o mesmo micros() da
// instância (cada placa tem o seu domínio de relógio, como no hardware).
#ifndef HOST_ESP_TIMER_H
#define HOST_ESP_TIMER_H

inline int64_t esp_timer_get_time() { return static_cast<int64_t>(micros()); }

#endif
