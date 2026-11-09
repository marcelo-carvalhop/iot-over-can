/*
 * Painel IoT-over-CAN na ESP32-2432S028 ("Cheap Yellow Display", CYD).
 *
 * O PanelApp (enlace, retrato, relevância, marcas, sinais) é o mesmo da
 * bancada virtual; aqui entra o hardware: LCD de 2,8" com 320x240 e toque
 * resistivo XPT2046, sem teclado, sem bateria e sem RTC. A tela não usa a
 * lista de desenho de 400x600 da página padrão: compõe a própria página, compacta, a partir da visão da
 * rede e dos itens de relevância (ioc_page_assess), e só redesenha o que mudou.
 *
 * Placa: a versão com USB-C e micro-USB usa ST7789 (padrão aqui, com cores
 * invertidas); a versão só com micro-USB usa ILI9341 (-D CYD_PANEL_ILI9341=1).
 * Se as barras de cor da partida saírem trocadas, ajuste CYD_INVERT ou
 * CYD_RGB_ORDER. A calibração do toque roda na primeira partida e sempre que
 * o botão BOOT estiver pressionado ao ligar.
 */
#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <stdarg.h>

#include "panel_app.h"

#ifndef CYD_PANEL_ILI9341
#define CYD_PANEL_ILI9341 0
#endif
#ifndef CYD_INVERT
#define CYD_INVERT (CYD_PANEL_ILI9341 ? 0 : 1)
#endif
#ifndef CYD_RGB_ORDER
#define CYD_RGB_ORDER 0
#endif
#ifndef CYD_ROTATION
#define CYD_ROTATION 1
#endif

namespace {

class Lcd : public lgfx::LGFX_Device {
#if CYD_PANEL_ILI9341
  lgfx::Panel_ILI9341 panel_;
#else
  lgfx::Panel_ST7789 panel_;
#endif
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_XPT2046 touch_;

 public:
  Lcd() {
    {
      auto c = bus_.config();
      c.spi_host = HSPI_HOST;  // tela: SCLK 14, MOSI 13, MISO 12, DC 2
      c.spi_mode = 0;
      c.freq_write = 40000000;
      c.freq_read = 16000000;
      c.spi_3wire = false;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 14;
      c.pin_mosi = 13;
      c.pin_miso = 12;
      c.pin_dc = 2;
      bus_.config(c);
      panel_.setBus(&bus_);
    }
    {
      auto c = panel_.config();
      c.pin_cs = 15;
      c.pin_rst = -1;
      c.pin_busy = -1;
      c.panel_width = 240;
      c.panel_height = 320;
      c.readable = true;
      c.invert = CYD_INVERT != 0;
      c.rgb_order = CYD_RGB_ORDER != 0;
      c.bus_shared = false;
      panel_.config(c);
    }
    {
      auto c = light_.config();
      c.pin_bl = 21;
      c.freq = 12000;
      c.pwm_channel = 7;
      light_.config(c);
      panel_.setLight(&light_);
    }
    {
      auto c = touch_.config();  // toque em barramento próprio: CLK 25, MOSI 32, MISO 39, CS 33
      c.x_min = 200;
      c.x_max = 3900;
      c.y_min = 200;
      c.y_max = 3900;
      c.pin_int = 36;
      c.bus_shared = false;
      c.spi_host = VSPI_HOST;
      c.freq = 1000000;
      c.pin_sclk = 25;
      c.pin_mosi = 32;
      c.pin_miso = 39;
      c.pin_cs = 33;
      touch_.config(c);
      panel_.setTouch(&touch_);
    }
    setPanel(&panel_);
  }
};

constexpr const char* SIM_MASTER_KEY = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
constexpr uint8_t PIN_LED_R = 4, PIN_LED_G = 16, PIN_LED_B = 17;  // LED RGB, ativo em nível baixo
constexpr uint8_t PIN_SPEAKER = 26;
constexpr uint8_t PIN_BOOT = 0;
constexpr int W = 320, HEADER_H = 34, FOOTER_Y = 196, BTN_W = 80;

constexpr uint32_t C_BG = 0x0F1C2Eu, C_FG = 0xFBFCFDu, C_MUTED = 0x93A1B5u, C_TAB = 0x1B2A40u,
                   C_TAB_ON = 0x2F4A73u, C_MARK = 0x12707Au, C_ACK = 0x9A5A0Au;

enum class Transport : uint8_t { Wifi, Usb };
enum class UiPage : uint8_t { Situation, Network };

Lcd lcd;
PanelApp app;
Preferences prefs;
Transport transport = Transport::Wifi;
uint64_t panelUuid = 0;
uint8_t networkKey[IOC_LINK_KEY_LEN] = {0};
bool haveNetworkKey = false;
bool usbLink = false;
WiFiUDP udp;
bool udpStarted = false;
uint8_t joinedNode = 0;
int64_t clockBase = 0;
uint32_t clockBaseMs = 0;
bool clockOk = false;
UiPage uiPage = UiPage::Situation;
bool uiDirty = true;
uint32_t uiHash = 0;

uint32_t levelColor(uint8_t level) {
  switch (level) {
    case IOC_LEVEL_HANDLING: return 0x2F4A73u;
    case IOC_LEVEL_ATTENTION: return 0xD9851Au;
    case IOC_LEVEL_YOU: return 0xB5382Au;
    default: return 0x2F7D52u;
  }
}

/* ---------------------------------------------------------- LED e som */

void ledInit() {
  const uint8_t pins[3] = {PIN_LED_R, PIN_LED_G, PIN_LED_B};
  for (uint8_t i = 0; i < 3; ++i) {
    ledcSetup(1 + i, 5000, 8);
    ledcAttachPin(pins[i], 1 + i);
    ledcWrite(1 + i, 255);
  }
}

void ledRgb(uint32_t rgb) {  // meia intensidade: o LED da CYD é forte
  ledcWrite(1, 255 - ((rgb >> 17) & 0x7F));
  ledcWrite(2, 255 - ((rgb >> 9) & 0x7F));
  ledcWrite(3, 255 - ((rgb >> 1) & 0x7F));
}

/* --------------------------------------------------------------- rádio */

portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t offerQueue[4][32];
uint8_t offerLen[4];
volatile uint8_t offerHead = 0, offerCount = 0;

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    const std::string md = dev->getManufacturerData();
    if (md.size() < 4 || md.size() > sizeof(offerQueue[0])) return;
    if (static_cast<uint8_t>(md[0]) != 0xFF || static_cast<uint8_t>(md[1]) != 0xFF || md[2] != 'I' || md[3] != 'O') return;
    portENTER_CRITICAL(&radioMux);
    if (offerCount < 4) {
      const uint8_t slot = (offerHead + offerCount) % 4;
      memcpy(offerQueue[slot], md.data(), md.size());
      offerLen[slot] = static_cast<uint8_t>(md.size());
      ++offerCount;
    }
    portEXIT_CRITICAL(&radioMux);
  }
} scanCallbacks;

void startBle() {
  NimBLEDevice::init("");
  uint8_t mfg[15] = {0xFF, 0xFF, 'I', 'C', 0x01, IOC_PANEL_PROFILE, IOC_LINK_VERSION};
  for (int i = 0; i < 8; ++i) mfg[7 + i] = static_cast<uint8_t>(panelUuid >> (56 - 8 * i));
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData data;
  data.setManufacturerData(std::string(reinterpret_cast<const char*>(mfg), sizeof(mfg)));
  adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  adv->setMinInterval(800);  // 500 ms
  adv->setMaxInterval(960);
  adv->setAdvertisementData(data);
  adv->start();
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);
  scan->setDuplicateFilter(0);
  scan->setActiveScan(false);
  scan->setInterval(800);
  scan->setWindow(96);  // 60 ms a cada 500 ms, como os sensores
  scan->start(0, false, true);
}

void stopRadios() {
  NimBLEDevice::getScan()->stop();
  NimBLEDevice::getAdvertising()->stop();
  udp.stop();
  udpStarted = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void printHex(const char* prefix, const uint8_t* data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  Serial.print(prefix);
  for (size_t i = 0; i < len; ++i) {
    Serial.write(hex[data[i] >> 4]);
    Serial.write(hex[data[i] & 15]);
  }
  Serial.println();
}

size_t parseHex(const char* text, uint8_t* out, size_t cap) {
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  size_t n = 0;
  while (text[0] && text[1] && n < cap) {
    const int hi = nib(text[0]), lo = nib(text[1]);
    if (hi < 0 || lo < 0) break;
    out[n++] = static_cast<uint8_t>((hi << 4) | lo);
    text += 2;
  }
  return n;
}

/* ------------------------------------------------------------------ HAL */

class CydHal : public PanelHal {
 public:
  uint32_t millis() override { return ::millis(); }
  int64_t epochNow() override { return clockBase + static_cast<int64_t>((::millis() - clockBaseMs) / 1000u); }
  bool clockValid() override { return clockOk; }
  void setClock(int64_t epoch) override {  // sem RTC: vale até desligar
    clockBase = epoch;
    clockBaseMs = ::millis();
    clockOk = true;
  }
  void joinNode(uint8_t node) override {
    joinedNode = node;
    if (transport == Transport::Usb) {
      Serial.printf("#IOC JOIN %u\n", node);
      return;
    }
    if (!haveNetworkKey) return;
    char ssid[IOC_LINK_SSID_LEN], psk[IOC_LINK_PSK_LEN];
    ioc_link_wifi_ssid(node, ssid);
    ioc_link_wifi_psk(networkKey, node, psk);
    udp.stop();  // não reutilizar o socket de uma associação anterior
    udpStarted = false;
    WiFi.disconnect();
    WiFi.begin(ssid, psk, ioc_link_wifi_channel(node));
  }
  void leaveNode() override {
    joinedNode = 0;
    if (transport == Transport::Usb) {
      Serial.println("#IOC LEAVE");
      return;
    }
    udp.stop();
    udpStarted = false;
    WiFi.disconnect();
  }
  bool linkUp() override {
    if (transport == Transport::Usb) return usbLink;
    if (WiFi.status() != WL_CONNECTED) {
      if (udpStarted) {
        udp.stop();
        udpStarted = false;
      }
      return false;
    }
    if (!udpStarted) udpStarted = udp.begin(IOC_LINK_UDP_PORT) == 1;
    return udpStarted;
  }
  bool sendDatagram(const uint8_t* data, size_t len) override {
    if (transport == Transport::Usb) {
      printHex("#IOC UP ", data, len);
      return true;
    }
    if (!linkUp()) return false;
    if (!udp.beginPacket(WiFi.gatewayIP(), IOC_LINK_UDP_PORT)) return false;
    udp.write(data, len);
    return udp.endPacket() == 1;
  }
  // A página padrão de 400x600 não é usada: o aviso só pede um redesenho.
  void render(const ioc_dl&) override { uiDirty = true; }
  bool renderBusy() override { return false; }
  void leds(uint32_t rgb0, uint32_t rgb1) override { ledRgb(rgb0 ? rgb0 : rgb1); }
  void tone(uint16_t hz, uint16_t ms) override { ::tone(PIN_SPEAKER, hz, ms); }
  void readSensors(uint8_t& pct, uint16_t& mv, int16_t& t, uint16_t& rh, bool& usb) override {
    pct = 255;  // sem bateria nem sensor de ambiente
    mv = 0;
    t = IOC_PANEL_NA_I16;
    rh = IOC_PANEL_NA_U16;
    usb = true;
  }
  bool keyboardPresent() override { return false; }
  void sleepFor(uint32_t seconds) override {  // não usado: canSleep = false
    stopRadios();
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
    esp_deep_sleep_start();
  }
  void powerOff() override {  // apaga tudo; um toque na tela religa
    Serial.println("#IOC OFF");
    stopRadios();
    ledRgb(0);
    lcd.setBrightness(0);
    lcd.sleep();
    Serial.flush();
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0);
    esp_deep_sleep_start();
  }
  void storeU32(const char* key, uint32_t value) override {
    if (prefs.getUInt(key, value ^ 1u) != value) prefs.putUInt(key, value);
  }
  uint32_t loadU32(const char* key, uint32_t fallback) override { return prefs.getUInt(key, fallback); }
  void storeStr(const char* key, const char* value) override { prefs.putString(key, value); }
  bool loadStr(const char* key, char* out, size_t cap) override { return prefs.getString(key, out, cap) > 0; }
  bool radioSurvey(char*, size_t) override { return false; }
  void log(const char* line) override { Serial.printf("#IOC LOG %s\n", line); }
} hal;

/* ----------------------------------------------------------------- tela */

// Corta o texto (UTF-8) para caber em max_w pixels, com reticências.
void fitText(char* s, size_t cap, int max_w) {
  if (lcd.textWidth(s) <= max_w) return;
  size_t n = strlen(s);
  char tmp[192];
  while (n > 0) {
    do { --n; } while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80);
    snprintf(tmp, sizeof(tmp), "%.*s...", static_cast<int>(n), s);
    if (lcd.textWidth(tmp) <= max_w) break;
  }
  snprintf(s, cap, "%s", tmp);
}

void drawText(const char* text, int x, int y, uint32_t fg, uint32_t bg, int max_w) {
  char buf[192];
  snprintf(buf, sizeof(buf), "%s", text);
  fitText(buf, sizeof(buf), max_w);
  lcd.setTextColor(fg, bg);
  lcd.drawString(buf, x, y);
}

void decimalComma(char* s) {
  for (; *s; ++s) if (*s == '.') *s = ',';
}

const char* profileName(uint8_t profile) {
  switch (profile) {
    case 0x01: return "vibração";
    case 0x02: return "acústico";
    case IOC_PANEL_PROFILE: return "painel";
    default: return "sensor";
  }
}

// Conteúdo da página em linhas (texto, cor); o hash evita redesenhos sem mudança.
struct Line { char text[160]; uint32_t color; uint8_t mark; uint8_t big; };
constexpr int MAX_LINES = 8;
Line lines[MAX_LINES];
int lineCount = 0;

void addLine(uint32_t color, uint8_t mark, uint8_t big, const char* fmt, ...) {
  if (lineCount >= MAX_LINES) return;
  Line& l = lines[lineCount++];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(l.text, sizeof(l.text), fmt, ap);
  va_end(ap);
  l.color = color;
  l.mark = mark;
  l.big = big;
}

void composeSituation(const ioc_view& v) {
  ioc_page_ctx ctx;
  app.fillCtx(ctx, ::millis());
  ioc_page_item items[3];
  const int n = ioc_page_assess(&v, &ctx, items, 3);
  for (int i = 0; i < n; ++i) {
    addLine(C_FG, static_cast<uint8_t>(1 + items[i].level), 0, "%s", items[i].title);
    if (items[i].detail[0]) addLine(C_MUTED, 0, 0, "%s", items[i].detail);
  }
  if (n > 0) return;
  addLine(0x5FBF8Au, 0, 1, "Tudo normal");
  uint8_t absent = 0;
  const uint8_t nodes = ioc_view_node_count(&v, &absent);
  if (v.leader) addLine(C_FG, 0, 0, "Líder: Módulo %02u", v.leader);
  addLine(C_FG, 0, 0, "Módulos: %u   Sensores: %u", nodes, ioc_view_sensor_count(&v));
  char load[24];
  snprintf(load, sizeof(load), "%.1f", v.load_pct);
  decimalComma(load);
  addLine(C_MUTED, 0, 0, "Ocupação do CAN: %s %%", load);
}

void composeNetwork(const ioc_view& v) {
  uint8_t absent = 0;
  const uint8_t nodes = ioc_view_node_count(&v, &absent);
  char load[24];
  snprintf(load, sizeof(load), "%.1f", v.load_pct);
  decimalComma(load);
  addLine(C_FG, 0, 0, "Líder %02u · %u módulos · CAN %s %%", v.leader, nodes, load);
  for (int i = 0; i < IOC_VIEW_SENSORS && lineCount < MAX_LINES; ++i) {
    const ioc_view_sensor& s = v.sensors[i];
    if (!s.used) continue;
    char value[24] = "";
    if (s.have_value && s.profile == 0x01) snprintf(value, sizeof(value), "%.2f m/s²", s.value);
    if (s.have_value && s.profile == 0x02) snprintf(value, sizeof(value), "%.0f dB", s.value);
    decimalComma(value);
    addLine(s.secure_flag ? C_FG : C_MUTED, 0, 0, "%02u.%02u %s%s%s · %d dBm", s.node, s.child, profileName(s.profile),
            value[0] ? " · " : "", value, s.rssi);
  }
  if (lineCount == 1) addLine(C_MUTED, 0, 0, "Nenhum sensor associado");
}

uint32_t hashLines(uint32_t h) {
  for (int i = 0; i < lineCount; ++i) {
    for (const char* p = lines[i].text; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
    h = (h ^ lines[i].color ^ (lines[i].mark << 24) ^ (lines[i].big << 28)) * 16777619u;
  }
  return h;
}

void drawButton(int i, const char* label, uint32_t bg) {
  lcd.fillRoundRect(i * BTN_W + 2, FOOTER_Y + 2, BTN_W - 4, 240 - FOOTER_Y - 4, 6, bg);
  lcd.setTextColor(C_FG, bg);
  lcd.drawCenterString(label, i * BTN_W + BTN_W / 2, FOOTER_Y + 13);
}

void drawUi(bool force) {
  const ioc_view& v = app.view();
  const uint8_t level = app.level();
  char where[24];
  if (transport == Transport::Usb) snprintf(where, sizeof(where), "USB");
  else if (app.sessionUp()) snprintf(where, sizeof(where), "Módulo %02u", app.parentNode());
  else if (!haveNetworkKey) snprintf(where, sizeof(where), "sem chave");
  else snprintf(where, sizeof(where), "%s", joinedNode ? "conectando" : "procurando");
  char right[40];
  if (hal.clockValid()) {  // hora local, vinda da rede ou da USB
    const int64_t local = hal.epochNow() + static_cast<int64_t>(app.tzMin()) * 60;
    const int s = static_cast<int>(((local % 86400) + 86400) % 86400);
    snprintf(right, sizeof(right), "%s  %02d:%02d", where, s / 3600, s / 60 % 60);
  } else {
    snprintf(right, sizeof(right), "%s", where);
  }
  lineCount = 0;
  if (!app.sessionUp() && !app.snapshotReceived()) {
    addLine(C_FG, 0, 1, "Sem dados da rede");
    if (joinedNode) addLine(C_MUTED, 0, 0, "Entrando no Wi-Fi do Módulo %02u", joinedNode);
    else addLine(C_MUTED, 0, 0, "Esperando a oferta de um módulo");
  } else if (uiPage == UiPage::Situation) {
    composeSituation(v);
  } else {
    composeNetwork(v);
  }
  uint32_t h = 2166136261u ^ (level << 8) ^ static_cast<uint8_t>(uiPage);
  for (const char* p = right; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
  h = hashLines(h);
  if (!force && h == uiHash) return;
  uiHash = h;

  lcd.startWrite();
  const uint32_t band = levelColor(level);
  lcd.fillRect(0, 0, W, HEADER_H, band);
  lcd.setFont(&fonts::efontJA_16);
  const int rightW = lcd.textWidth(right);
  lcd.setTextColor(C_FG, band);
  lcd.drawString(right, W - 6 - rightW, 9);
  lcd.setFont(&fonts::efontJA_24);
  drawText(ioc_page_level_title(level), 6, 5, C_FG, band, W - 20 - rightW);

  lcd.fillRect(0, HEADER_H, W, FOOTER_Y - HEADER_H, C_BG);
  int y = HEADER_H + 6;
  for (int i = 0; i < lineCount && y < FOOTER_Y - 16; ++i) {
    const Line& l = lines[i];
    lcd.setFont(l.big ? &fonts::efontJA_24 : &fonts::efontJA_16);
    int x = 8;
    if (l.mark) {
      lcd.fillRect(8, y + 4, 8, 8, levelColor(static_cast<uint8_t>(l.mark - 1)));
      x = 22;
    } else if (i > 0 && !lines[i - 1].big && lines[i - 1].mark) {
      x = 22;  // detalhe do item acima
    }
    drawText(l.text, x, y, l.color, C_BG, W - x - 6);
    y += l.big ? 32 : (l.mark ? 18 : 22);
  }

  lcd.fillRect(0, FOOTER_Y, W, 240 - FOOTER_Y, C_BG);
  lcd.setFont(&fonts::efontJA_16);
  drawButton(0, "Situação", uiPage == UiPage::Situation ? C_TAB_ON : C_TAB);
  drawButton(1, "Rede", uiPage == UiPage::Network ? C_TAB_ON : C_TAB);
  drawButton(2, "Marcar", C_MARK);
  drawButton(3, "Ciente", C_ACK);
  lcd.endWrite();
}

void onTap(int32_t x, int32_t y) {
  if (y < FOOTER_Y) return;
  switch (x / BTN_W) {
    case 0: uiPage = UiPage::Situation; break;
    case 1: uiPage = UiPage::Network; break;
    case 2: app.onButton('A'); break;      // marca o instante (ou dá ciência)
    default: app.onPowerClick(false); break;  // ciência (ou atualiza)
  }
  uiDirty = true;
}

void pollTouch() {
  static bool down = false;
  static uint32_t lastMs = 0;
  int32_t x = 0, y = 0;
  const bool pressed = lcd.getTouch(&x, &y) > 0;
  if (pressed && !down && ::millis() - lastMs > 200) {
    lastMs = ::millis();
    onTap(x, y);
  }
  down = pressed;
}

// Barras de cor na partida: confirmam o controlador, a inversão e a ordem RGB.
void bootScreen() {
  lcd.fillScreen(C_BG);
  lcd.setFont(&fonts::efontJA_24);
  lcd.setTextColor(C_FG, C_BG);
  lcd.drawString("iot-over-can", 8, 8);
  lcd.setFont(&fonts::efontJA_16);
  lcd.setTextColor(C_MUTED, C_BG);
  lcd.drawString("Painel CYD fw " PANEL_FW_VERSION, 8, 40);
  const uint32_t colors[4] = {0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFFFFu};
  const char* names[4] = {"vermelho", "verde", "azul", "branco"};
  for (int i = 0; i < 4; ++i) {
    lcd.fillRect(i * BTN_W, 80, BTN_W, 90, colors[i]);
    lcd.setTextColor(C_FG, C_BG);
    lcd.drawCenterString(names[i], i * BTN_W + BTN_W / 2, 180);
  }
  lcd.drawString(CYD_PANEL_ILI9341 ? "ILI9341" : "ST7789", 8, 210);
  delay(1500);
}

void calibrateTouch() {
  uint16_t cal[8];
  pinMode(PIN_BOOT, INPUT_PULLUP);
  const bool forced = digitalRead(PIN_BOOT) == LOW;
  if (!forced && prefs.getBytes("tcal", cal, sizeof(cal)) == sizeof(cal)) {
    lcd.setTouchCalibrate(cal);
    return;
  }
  lcd.fillScreen(C_BG);
  lcd.setFont(&fonts::efontJA_16);
  lcd.setTextColor(C_FG, C_BG);
  lcd.drawCenterString("Calibração do toque", W / 2, 96);
  lcd.drawCenterString("toque a ponta de cada seta", W / 2, 120);
  lcd.calibrateTouch(cal, C_FG, C_BG, 16);
  prefs.putBytes("tcal", cal, sizeof(cal));
  lcd.setTouchCalibrate(cal);
  Serial.println("#IOC LOG toque calibrado e gravado");
}

/* ---------------------------------------------------------- configuração */

void configureApp(Transport t) {
  transport = t;
  PanelConfig cfg;
  cfg.uuid = panelUuid;
  cfg.canSleep = false;  // sempre na USB
  uint8_t master[IOC_LINK_KEY_LEN];
  haveNetworkKey = false;
  if (t == Transport::Usb) {
    ioc_link_hex_to_key(SIM_MASTER_KEY, master);
    ioc_link_derive_device_key(master, panelUuid, cfg.devKey);
    cfg.userBoot = false;
  } else {
#if defined(IOT_PANEL_DEVICE_KEY_HEX)
    ioc_link_hex_to_key(IOT_PANEL_DEVICE_KEY_HEX, cfg.devKey);
    haveNetworkKey = ioc_link_hex_to_key(IOT_PANEL_NETWORK_KEY_HEX, networkKey) != 0;
#elif defined(IOT_PANEL_MASTER_KEY_HEX)
    if (ioc_link_hex_to_key(IOT_PANEL_MASTER_KEY_HEX, master)) {
      ioc_link_derive_device_key(master, panelUuid, cfg.devKey);
      ioc_link_derive_network_key(master, networkKey);
      haveNetworkKey = true;
    }
#endif
  }
  app.begin(&hal, cfg);
  uiDirty = true;
}

void handleHostLine(char* line) {
  if (strncmp(line, "#IOC ", 5) != 0) return;
  char* cmd = line + 5;
  static uint8_t buf[IOC_PANEL_DATAGRAM_MAX + IOC_ENV_OVERHEAD];
  if (strncmp(cmd, "BRIDGE", 6) == 0) {
    if (transport == Transport::Usb) {
      usbLink = false;
      app.restartLink();
    } else {
      stopRadios();
      usbLink = false;
      configureApp(Transport::Usb);
    }
    Serial.printf("#IOC READY uuid=0x%016llX profile=PANEL fw=%s\n", static_cast<unsigned long long>(panelUuid),
                  PANEL_FW_VERSION);
  } else if (strncmp(cmd, "TIME ", 5) == 0) {
    const int64_t epoch = atoll(cmd + 5);
    app.setClockFrom(epoch, IOC_TIME_SRC_TERMINAL);
  } else if (strncmp(cmd, "LINK ", 5) == 0) {
    usbLink = cmd[5] == '1';
  } else if (strncmp(cmd, "OFFER ", 6) == 0) {
    app.onOffer(buf, parseHex(cmd + 6, buf, sizeof(buf)));
  } else if (strncmp(cmd, "DOWN ", 5) == 0) {
    app.onDatagram(buf, parseHex(cmd + 5, buf, sizeof(buf)));
  } else if (strncmp(cmd, "BUTTON ", 7) == 0) {
    if (cmd[7] == 'P') app.onPowerClick(false);
    else app.onButton(cmd[7]);
  }
}

void pollSerial() {
  static char line[2 * (IOC_PANEL_DATAGRAM_MAX + IOC_ENV_OVERHEAD) + 32];
  static size_t len = 0;
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len) {
        line[len] = '\0';
        handleHostLine(line);
        len = 0;
      }
    } else if (len < sizeof(line) - 1) {
      line[len++] = static_cast<char>(c);
    } else {
      len = 0;
    }
  }
}

void pollRadio() {
  if (transport != Transport::Wifi) return;
  for (;;) {
    uint8_t mfg[32];
    uint8_t len = 0;
    portENTER_CRITICAL(&radioMux);
    if (offerCount) {
      len = offerLen[offerHead];
      memcpy(mfg, offerQueue[offerHead], len);
      offerHead = (offerHead + 1) % 4;
      --offerCount;
    }
    portEXIT_CRITICAL(&radioMux);
    if (!len) break;
    app.onOffer(mfg, len);
  }
  if (!udpStarted) return;
  static uint8_t rx[IOC_PANEL_DATAGRAM_MAX + IOC_ENV_OVERHEAD];
  for (int i = 0; i < 4; ++i) {
    const int size = udp.parsePacket();
    if (size <= 0) break;
    const int got = udp.read(rx, sizeof(rx));
    if (got > 0 && size <= static_cast<int>(sizeof(rx))) app.onDatagram(rx, static_cast<size_t>(got));
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  prefs.begin("iocpanel", false);
  lcd.init();
  lcd.setRotation(CYD_ROTATION);
  lcd.setBrightness(200);
  bootScreen();
  calibrateTouch();
  ledInit();
#if defined(IOT_PANEL_UUID)
  panelUuid = IOT_PANEL_UUID;
#else
  panelUuid = 0x4359000000000000ULL | (ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL);  // "CY" + MAC
#endif
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  startBle();
  configureApp(Transport::Wifi);
  esp_task_wdt_init(60, true);
  esp_task_wdt_add(nullptr);
  Serial.printf("#IOC HELLO uuid=0x%016llX profile=PANEL fw=%s keys=%s board=CYD\n",
                static_cast<unsigned long long>(panelUuid), PANEL_FW_VERSION, haveNetworkKey ? "LINK" : "NONE");
}

void loop() {
  esp_task_wdt_reset();
  pollTouch();
  pollSerial();
  pollRadio();
  app.loop();
  static uint32_t lastUi = 0;
  if (uiDirty || millis() - lastUi >= 1000) {
    lastUi = millis();
    drawUi(uiDirty);
    uiDirty = false;
  }
  static uint32_t lastHello = 0;
  if (millis() - lastHello > (transport == Transport::Usb ? 10000u : 2000u)) {
    lastHello = millis();
    Serial.printf("#IOC HELLO uuid=0x%016llX profile=PANEL fw=%s mode=%s session=%u node=%u board=CYD\n",
                  static_cast<unsigned long long>(panelUuid), PANEL_FW_VERSION, transport == Transport::Usb ? "USB" : "WIFI",
                  app.sessionUp() ? 1u : 0u, app.parentNode());
  }
  delay(5);
}
