/* M5Unified de mentira: a tela confere se cada desenho cabe nos 400x600 e se
 * nenhum byte de controle chega a ser escrito. A largura do texto segue a efont
 * (largura fixa: 6, 8 ou 12 px por caractere). */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>

#include "Arduino.h"

namespace lgfx {
struct IFont {
  int largura;
  int altura;
};
struct U8g2font : IFont {};
}  // namespace lgfx

namespace fonts {
inline const lgfx::U8g2font efontJA_12{{6, 12}};
inline const lgfx::U8g2font efontJA_16{{8, 16}};
inline const lgfx::U8g2font efontJA_16_b{{8, 16}};
inline const lgfx::U8g2font efontJA_16_i{{8, 16}};
inline const lgfx::U8g2font efontJA_24_b{{12, 24}};
}  // namespace fonts

enum class epd_mode_t { epd_quality, epd_text, epd_fast, epd_fastest };

namespace stub {
inline std::atomic<int> foraDaTela{0};
inline std::atomic<int> controleNaTela{0};
inline std::atomic<int> noPapel{0};  // chamadas a display()
inline std::string ultimoErro;
inline bool autoDisplay = true;
inline std::atomic<int> vbus{0};
inline std::atomic<bool> desligado{false};
inline bool teclado = false;
inline uint8_t tecla = 0;
}  // namespace stub

struct DisplayStub {
  const lgfx::IFont* fonte = &fonts::efontJA_16;
  static uint32_t color888(uint8_t r, uint8_t g, uint8_t b) { return (uint32_t(r) << 16) | (uint32_t(g) << 8) | b; }
  void confere(int x, int y, int w, int h, const char* o) {
    if (x < 0 || y < 0 || x + w > 400 || y + h > 600) {
      ++stub::foraDaTela;
      char buf[200];
      std::snprintf(buf, sizeof(buf), "%s em (%d,%d) %dx%d", o, x, y, w, h);
      stub::ultimoErro = buf;
    }
  }
  void fillScreen(uint32_t) {}
  void setFont(const lgfx::IFont* f) { fonte = f; }
  void setTextColor(uint32_t) {}
  void setRotation(int) {}
  void setEpdMode(epd_mode_t) {}
  void setAutoDisplay(bool v) { stub::autoDisplay = v; }
  void setTextWrap(bool) {}
  void startWrite() {}
  void endWrite() {}
  void display() { ++stub::noPapel; }
  void waitDisplay() {}
  void sleep() {}
  int32_t textWidth(const char* s) const {
    int32_t n = 0;
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(s); *c; ++c)
      if ((*c & 0xC0) != 0x80) ++n;
    return n * fonte->largura;
  }
  size_t drawString(const char* s, int32_t x, int32_t y) {
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(s); *c; ++c)
      if (*c < 0x20) {
        ++stub::controleNaTela;
        stub::ultimoErro = std::string("byte de controle em \"") + s + "\"";
      }
    confere(x, y, textWidth(s), fonte->altura, s);
    return 0;
  }
  void drawFastHLine(int x, int y, int w, uint32_t) { confere(x, y, w, 1, "linha"); }
  void fillRect(int x, int y, int w, int h, uint32_t) { confere(x, y, w, h, "retângulo"); }
  void fillCircle(int x, int y, int r, uint32_t) { confere(x - r, y - r, 2 * r + 1, 2 * r + 1, "círculo"); }
  void drawCircle(int x, int y, int r, uint32_t c) { fillCircle(x, y, r, c); }
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t) {
    const int x = std::min(x0, std::min(x1, x2)), y = std::min(y0, std::min(y1, y2));
    confere(x, y, std::max(x0, std::max(x1, x2)) - x + 1, std::max(y0, std::max(y1, y2)) - y + 1, "triângulo");
  }
};

struct BotaoStub {
  bool clique = false, segura = false, proximoClique = false, proximoSegura = false;
  bool wasClicked() const { return clique; }
  bool wasHold() const { return segura; }
  void setHoldThresh(uint32_t) {}
  void avanca() {
    clique = proximoClique;
    segura = proximoSegura;
    proximoClique = proximoSegura = false;
  }
};

struct I2CStub {
  bool begin() { return true; }
  bool scanID(uint8_t, uint32_t) const { return stub::teclado; }
  bool start(uint8_t, bool, uint32_t) { return stub::teclado; }
  bool read(uint8_t* b, size_t, bool) {
    *b = stub::tecla;
    stub::tecla = 0;
    return true;
  }
  bool stop() { return true; }
  bool bitOff(uint8_t, uint8_t, uint8_t, uint32_t) { return true; }
};

struct M5pm1Stub {
  void setLDOOutput(bool) {}
  void powerOff() { stub::desligado = true; }
};

struct PowerStub {
  M5pm1Stub M5pm1;
  int32_t getBatteryLevel() { return 80; }
  int16_t getVBUSVoltage() { return static_cast<int16_t>(stub::vbus.load()); }
  void setExtOutput(bool) {}
};

struct LedStub {
  void setAutoDisplay(bool) {}
  void setAllColor(uint8_t, uint8_t, uint8_t) {}
  void display() {}
};

struct ConfigStub {
  uint32_t serial_baudrate = 0;
  bool clear_display = true, output_power = false, internal_imu = true, internal_mic = true, internal_spk = true;
};

struct M5Stub {
  DisplayStub Display;
  BotaoStub BtnA, BtnB, BtnC;
  I2CStub Ex_I2C, In_I2C;
  PowerStub Power;
  LedStub Led;
  ConfigStub config() { return {}; }
  void begin(const ConfigStub&) {}
  void update() {
    BtnA.avanca();
    BtnB.avanca();
    BtnC.avanca();
  }
};
inline M5Stub M5;
