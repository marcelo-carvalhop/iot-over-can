// Substitutos mínimos do núcleo Arduino-ESP32 para compilar e exercitar
// wireless_link.cpp no computador de desenvolvimento. Não fazem parte do
// firmware; ver Codigo/node-can/test/host/README.md.
#ifndef HOST_ARDUINO_H
#define HOST_ARDUINO_H

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

typedef uint8_t byte;

uint32_t millis();
void host_set_millis(uint32_t value);

class HostSerial {
 public:
  std::string captured;
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void println(const char* text = "");
  void print(const char* text);
};
extern HostSerial Serial;

class IPAddress {
 public:
  IPAddress() : value_(0) {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
      : value_((uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | d) {}
  bool operator==(const IPAddress& other) const { return value_ == other.value_; }

 private:
  uint32_t value_;
};

void esp_fill_random(void* buf, size_t len);

#endif
