// Substitutos mínimos do núcleo Arduino-ESP32 para compilar e exercitar o
// firmware do Node CAN no computador de desenvolvimento. Não fazem parte do
// firmware; ver Codigo/node-can/test/host/README.md.
//
// Na bancada virtual (sim_network.cpp) estes arquivos são incluídos dentro de
// um namespace por instância de Node; por isso não declaram nada no escopo
// global de forma explícita e dependem de os cabeçalhos da biblioteca padrão
// já terem sido incluídos antes (sim_prelude.h).
#ifndef HOST_ARDUINO_H
#define HOST_ARDUINO_H

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

typedef uint8_t byte;

#define HEX 16
#define DEC 10
#define OUTPUT 1
#define LOW 0
#define HIGH 1

uint32_t millis();
uint32_t micros();
void delay(uint32_t ms);
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}

// Seções críticas do FreeRTOS: no host há uma única linha de execução.
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))

class HostSerial {
 public:
  std::string captured;  // tudo o que o firmware escreveu
  std::string input;     // texto aguardando leitura pelo firmware
  size_t inputPos = 0;

  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void println(const char* text = "");
  void print(const char* text);

  template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
  void print(T value, int base = DEC) {
    char buf[32];
    if (std::is_floating_point<T>::value) {
      snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(value));
    } else if (base == HEX) {
      snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(value));
    } else if (std::is_signed<T>::value) {
      snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
    } else {
      snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
    }
    print(buf);
  }
  template <typename T, typename = typename std::enable_if<std::is_arithmetic<T>::value>::type>
  void println(T value, int base = DEC) {
    print(value, base);
    println();
  }

  void begin(unsigned long) {}
  void setTxBufferSize(size_t) {}
  void flush() {}
  int available() { return static_cast<int>(input.size() - inputPos); }
  int read() {
    if (inputPos >= input.size()) return -1;
    const int c = static_cast<unsigned char>(input[inputPos++]);
    if (inputPos == input.size()) {
      input.clear();
      inputPos = 0;
    }
    return c;
  }
};
extern HostSerial Serial;

class HostEsp {
 public:
  void restart();  // definido pelo ambiente de cada simulação
};
extern HostEsp ESP;

class IPAddress {
 public:
  IPAddress() : value_(0) {}
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
      : value_((uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | d) {}
  bool operator==(const IPAddress& other) const { return value_ == other.value_; }
  uint8_t operator[](int index) const { return (uint8_t)(value_ >> (24 - 8 * index)); }

 private:
  uint32_t value_;
};

void esp_fill_random(void* buf, size_t len);

#endif
