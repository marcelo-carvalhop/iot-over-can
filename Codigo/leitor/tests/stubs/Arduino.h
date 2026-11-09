/* Arduino e FreeRTOS de mentira para rodar o firmware do leitor no computador
 * (tests/test_firmware.cpp). O relógio é controlado pelo teste; a tarefa de
 * desenho roda numa thread de verdade. */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace stub {
inline std::atomic<uint32_t> agoraMs{0};
inline std::mutex mutexCritico;
inline std::mutex mutexAviso;
inline std::condition_variable aviso;
inline uint32_t avisos = 0;
inline bool tarefaCriada = false;
}  // namespace stub

inline uint32_t millis() { return stub::agoraMs.load(); }
inline void delay(uint32_t ms) { stub::agoraMs += ms; }

struct SerialStub {
  std::mutex m;
  std::string entrada;
  std::string saida;
  bool eco = false;
  int printf(const char* f, ...) __attribute__((format(printf, 2, 3))) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    const int n = std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    escreve(buf);
    return n;
  }
  void println(const char* s) {
    escreve(s);
    escreve("\n");
  }
  void flush() {}
  int available() {
    std::lock_guard<std::mutex> g(m);
    return static_cast<int>(entrada.size());
  }
  int read() {
    std::lock_guard<std::mutex> g(m);
    if (entrada.empty()) return -1;
    const int c = static_cast<unsigned char>(entrada[0]);
    entrada.erase(0, 1);
    return c;
  }
  void escreve(const char* s) {
    std::lock_guard<std::mutex> g(m);
    saida += s;
    if (eco) std::fputs(s, stdout);
  }
};
inline SerialStub Serial;

typedef void* TaskHandle_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdTRUE 1
#define portMAX_DELAY 0xFFFFFFFFu
struct portMUX_TYPE {
  int livre;
};
#define portMUX_INITIALIZER_UNLOCKED {0}
inline void portENTER_CRITICAL(portMUX_TYPE*) { stub::mutexCritico.lock(); }
inline void portEXIT_CRITICAL(portMUX_TYPE*) { stub::mutexCritico.unlock(); }

inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char*, uint32_t, void* arg, UBaseType_t,
                                          TaskHandle_t* h, BaseType_t) {
  if (!stub::tarefaCriada) {
    stub::tarefaCriada = true;
    std::thread(fn, arg).detach();
  }
  if (h) *h = reinterpret_cast<TaskHandle_t>(1);
  return 1;
}
inline uint32_t ulTaskNotifyTake(BaseType_t, uint32_t) {
  std::unique_lock<std::mutex> g(stub::mutexAviso);
  stub::aviso.wait(g, [] { return stub::avisos > 0; });
  const uint32_t n = stub::avisos;
  stub::avisos = 0;
  return n;
}
inline BaseType_t xTaskNotifyGive(TaskHandle_t) {
  {
    std::lock_guard<std::mutex> g(stub::mutexAviso);
    ++stub::avisos;
  }
  stub::aviso.notify_one();
  return 1;
}
