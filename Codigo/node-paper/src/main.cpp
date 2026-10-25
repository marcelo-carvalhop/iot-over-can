/*
 * Firmware do painel em papel eletrônico (M5Stack PaperColor).
 *
 * Este arquivo é a camada de hardware: M5Unified (tela Spectra 6, botões,
 * energia M5PM1, LEDs, alto-falante, RTC), SHT40, teclado CardKB, BLE
 * (anúncio do perfil e ofertas), Wi-Fi (ponto de acesso do Node) e a USB.
 * A lógica está em panel_app.cpp, a mesma que roda na bancada virtual.
 *
 * Dois transportes para o mesmo enlace autenticado:
 *  - Wi-Fi: o painel anuncia o perfil por BLE, recebe a oferta assinada do
 *    Node responsável, entra no ponto de acesso IOC-NN e troca datagramas UDP;
 *  - USB: a ponte Simulacao/usb_panel_bridge.py leva os mesmos datagramas ao
 *    Node virtual do Simulation Studio ("#IOC UP/DOWN <hex>"), como um canal
 *    Wi-Fi. A troca acontece quando a ponte envia "#IOC BRIDGE".
 *
 * Energia: um toque no botão liga; com o painel ligado, um toque atualiza e
 * dois toques desligam de forma ordenada (página "Painel desligado", aviso
 * ao Node). Na bateria, entre verificações, o M5PM1 desliga tudo e o RTC
 * religa o painel (timerSleep).
 */
#include <Arduino.h>
#include <M5Unified.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>
#include <time.h>

#include "panel_app.h"

namespace {

// Chave pública da bancada virtual (build_network_sim.sh). Nunca usada em hardware de campo.
constexpr const char* SIM_MASTER_KEY = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
constexpr uint8_t CARDKB_ADDR = 0x5F;
constexpr uint8_t SHT40_ADDR = 0x44;
constexpr uint8_t M5PM1_ADDR = 0x6E;
constexpr uint32_t I2C_FREQ = 100000;

enum class Transport : uint8_t { Wifi, Usb };

PanelApp app;
Preferences prefs;
Transport transport = Transport::Wifi;
uint64_t panelUuid = 0;
uint8_t networkKey[IOC_LINK_KEY_LEN] = {0};
bool haveNetworkKey = false;
bool usbLink = false;
bool bleStarted = false;
WiFiUDP udp;
bool udpStarted = false;
uint8_t joinedNode = 0;

/* ------------------------------------------------------------- relógio */

int64_t clockBase = 0;
uint32_t clockBaseMs = 0;
bool clockOk = false;

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void loadClock() {
  m5::rtc_datetime_t dt;
  if (M5.Rtc.isEnabled() && M5.Rtc.getDateTime(&dt) && dt.date.month >= 1) {
    clockBase = daysFromCivil(dt.date.year, static_cast<unsigned>(dt.date.month), static_cast<unsigned>(dt.date.date)) * 86400 +
                dt.time.hours * 3600 + dt.time.minutes * 60 + dt.time.seconds;
    clockOk = dt.date.year >= 2025 && prefs.getUInt("clk", 0) == 1;
  }
  clockBaseMs = millis();
}

/* -------------------------------------------------------------- desenho */

ioc_dl renderDl;
volatile bool renderPending = false;
volatile bool renderActive = false;

uint32_t inkColor(uint8_t ink) {
  // Cores exatas da paleta do Panel_ED2208: sem pontilhamento.
  switch (ink) {
    case IOC_INK_WHITE: return M5.Display.color888(255, 255, 255);
    case IOC_INK_YELLOW: return M5.Display.color888(255, 243, 56);
    case IOC_INK_RED: return M5.Display.color888(191, 0, 0);
    case IOC_INK_BLUE: return M5.Display.color888(100, 64, 255);
    case IOC_INK_GREEN: return M5.Display.color888(67, 138, 28);
    default: return M5.Display.color888(0, 0, 0);
  }
}

void thickLine(int x0, int y0, int x1, int y1, uint32_t c) {
  for (int d = -1; d <= 1; ++d) {
    M5.Display.drawLine(x0 + d, y0, x1 + d, y1, c);
    M5.Display.drawLine(x0, y0 + d, x1, y1 + d, c);
  }
}

void drawSymbol(const ioc_dl_item& it) {
  const int s = it.size, x = it.x, y = it.y;
  const uint32_t c = inkColor(it.ink);
  switch (it.w) {
    case IOC_SYM_DOT: M5.Display.fillCircle(x + s / 2, y + s / 2, s / 2 - 1, c); break;
    case IOC_SYM_TRIANGLE: M5.Display.fillTriangle(x + s / 2, y, x, y + s - 1, x + s - 1, y + s - 1, c); break;
    case IOC_SYM_CROSS:
      thickLine(x + 2, y + 2, x + s - 3, y + s - 3, c);
      thickLine(x + s - 3, y + 2, x + 2, y + s - 3, c);
      break;
    case IOC_SYM_CHECK:
      thickLine(x + 1, y + s / 2, x + s / 3, y + s - 3, c);
      thickLine(x + s / 3, y + s - 3, x + s - 2, y + 2, c);
      break;
    default:
      M5.Display.fillTriangle(x + s / 2, y, x, y + s * 3 / 4, x + s - 1, y + s * 3 / 4, c);
      M5.Display.fillTriangle(x + s / 2, y + s - 1, x, y + s / 4, x + s - 1, y + s / 4, c);
      break;
  }
}

void drawPage(const ioc_dl& dl) {
  M5.Display.startWrite();
  M5.Display.fillScreen(inkColor(IOC_INK_WHITE));
  for (uint16_t i = 0; i < dl.count; ++i) {
    const ioc_dl_item& it = dl.item[i];
    const uint32_t c = inkColor(it.ink);
    switch (it.op) {
      case IOC_DL_FILL: M5.Display.fillRect(it.x, it.y, it.w, it.h, c); break;
      case IOC_DL_FRAME: M5.Display.drawRect(it.x, it.y, it.w, it.h, c); break;
      case IOC_DL_LINE: M5.Display.drawLine(it.x, it.y, it.x + it.w, it.y + it.h, c); break;
      case IOC_DL_SYMBOL: drawSymbol(it); break;
      case IOC_DL_TEXT_OP:
        if (it.text >= dl.text_len) break;
        M5.Display.setFont(it.size >= 24 && it.size < 32 ? &fonts::efontJA_24 : &fonts::efontJA_16);
        M5.Display.setTextSize(it.size >= 32 ? 2 : 1);
        M5.Display.setTextColor(c);
        M5.Display.drawString(dl.text + it.text, it.x, it.y);
        break;
      default: break;
    }
  }
  M5.Display.endWrite();
  M5.Display.display();
  M5.Display.waitDisplay();
}

// O papel leva 15 a 30 s: a tarefa de desenho roda no núcleo 0 para que o
// enlace (PING a cada 3 s, sessão de 10 s) continue respondendo.
void renderTask(void*) {
  for (;;) {
    if (renderPending) {
      renderPending = false;
      drawPage(renderDl);
      renderActive = false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

/* ------------------------------------------------------------- sensores */

uint8_t batteryPct = 255;
uint16_t batteryMv = 0;
int16_t tempC10 = IOC_PANEL_NA_I16;
uint16_t rh10 = IOC_PANEL_NA_U16;
bool onUsb = false;
uint32_t sensorsReadMs = 0;
bool keyboard = false;
uint32_t keyboardProbeMs = 0;

void readSht40() {
  uint8_t cmd = 0xFD, raw[6];
  if (!M5.In_I2C.start(SHT40_ADDR, false, I2C_FREQ)) return;
  M5.In_I2C.write(cmd);
  M5.In_I2C.stop();
  delay(10);
  if (!M5.In_I2C.start(SHT40_ADDR, true, I2C_FREQ)) return;
  const bool ok = M5.In_I2C.read(raw, sizeof(raw), true);
  M5.In_I2C.stop();
  if (!ok) return;
  const float t = -45.0f + 175.0f * static_cast<float>((raw[0] << 8) | raw[1]) / 65535.0f;
  float rh = -6.0f + 125.0f * static_cast<float>((raw[3] << 8) | raw[4]) / 65535.0f;
  rh = rh < 0 ? 0 : (rh > 100 ? 100 : rh);
  tempC10 = static_cast<int16_t>(t * 10.0f);
  rh10 = static_cast<uint16_t>(rh * 10.0f);
}

void refreshSensors(bool force) {
  if (!force && millis() - sensorsReadMs < 30000) return;
  sensorsReadMs = millis();
  const int32_t level = M5.Power.getBatteryLevel();
  batteryPct = level >= 0 && level <= 100 ? static_cast<uint8_t>(level) : 255;
  const int16_t mv = M5.Power.getBatteryVoltage();
  batteryMv = mv > 0 ? static_cast<uint16_t>(mv) : 0;
  onUsb = M5.Power.getVBUSVoltage() > 4300;
  readSht40();
}

void probeKeyboard() {
  if (millis() - keyboardProbeMs < 5000 && keyboardProbeMs) return;
  keyboardProbeMs = millis();
  keyboard = M5.Ex_I2C.start(CARDKB_ADDR, true, I2C_FREQ);
  M5.Ex_I2C.stop();
}

uint8_t readKey() {
  if (!keyboard) return 0;
  uint8_t c = 0;
  if (!M5.Ex_I2C.start(CARDKB_ADDR, true, I2C_FREQ)) { keyboard = false; return 0; }
  M5.Ex_I2C.read(&c, 1, true);
  M5.Ex_I2C.stop();
  return c;
}

/* --------------------------------------------------------------- rádio */

struct Seen { uint64_t uuid; int8_t rssi; uint8_t profile; uint32_t ms; };
constexpr uint8_t SEEN_MAX = 12;
Seen seen[SEEN_MAX];
portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t offerQueue[4][32];
uint8_t offerLen[4];
volatile uint8_t offerHead = 0, offerCount = 0;

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    const std::string md = dev->getManufacturerData();
    if (md.size() < 4 || static_cast<uint8_t>(md[0]) != 0xFF || static_cast<uint8_t>(md[1]) != 0xFF) return;
    portENTER_CRITICAL(&radioMux);
    if (md[2] == 'I' && md[3] == 'O' && md.size() <= sizeof(offerQueue[0]) && offerCount < 4) {
      const uint8_t slot = (offerHead + offerCount) % 4;
      memcpy(offerQueue[slot], md.data(), md.size());
      offerLen[slot] = static_cast<uint8_t>(md.size());
      ++offerCount;
    } else if (md[2] == 'I' && md[3] == 'C' && md.size() >= 15) {
      uint64_t uuid = 0;
      for (int i = 0; i < 8; ++i) uuid = (uuid << 8) | static_cast<uint8_t>(md[7 + i]);
      Seen* slot = &seen[0];
      for (Seen& s : seen) {
        if (s.uuid == uuid) { slot = &s; break; }
        if (s.ms < slot->ms) slot = &s;
      }
      slot->uuid = uuid;
      slot->rssi = static_cast<int8_t>(dev->getRSSI());
      slot->profile = static_cast<uint8_t>(md[5]);
      slot->ms = millis();
    }
    portEXIT_CRITICAL(&radioMux);
  }
} scanCallbacks;

void startBle() {
  NimBLEDevice::init("");
  bleStarted = true;
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
  scan->setWindow(96);  // 60 ms a cada 500 ms, como o Pico W
  scan->start(0, false, true);
}

void stopRadios() {
  if (bleStarted) {
    NimBLEDevice::getScan()->stop();
    NimBLEDevice::getAdvertising()->stop();
  }
  udp.stop();
  udpStarted = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

/* --------------------------------------------------------- utilidades USB */

void printHex(const char* prefix, const uint8_t* data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  Serial.print(prefix);
  char chunk[129];
  size_t n = 0;
  for (size_t i = 0; i < len; ++i) {
    chunk[n++] = hex[data[i] >> 4];
    chunk[n++] = hex[data[i] & 15];
    if (n == 128) { chunk[n] = '\0'; Serial.print(chunk); n = 0; }
  }
  chunk[n] = '\0';
  Serial.println(chunk);
}

size_t parseHex(const char* text, uint8_t* out, size_t cap) {
  size_t n = 0;
  while (text[0] && text[1] && n < cap) {
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int hi = nib(text[0]), lo = nib(text[1]);
    if (hi < 0 || lo < 0) break;
    out[n++] = static_cast<uint8_t>((hi << 4) | lo);
    text += 2;
  }
  return n;
}

/* ------------------------------------------------------------------ HAL */

class DeviceHal : public PanelHal {
 public:
  uint32_t millis() override { return ::millis(); }
  int64_t epochNow() override { return clockBase + static_cast<int64_t>((::millis() - clockBaseMs) / 1000u); }
  bool clockValid() override { return clockOk; }
  void setClock(int64_t epoch) override {
    const time_t t = static_cast<time_t>(epoch);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    M5.Rtc.setDateTime(&tmv);
    clockBase = epoch;
    clockBaseMs = ::millis();
    clockOk = true;
    prefs.putUInt("clk", 1);
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
    // WiFiUDP não deve reutilizar o socket de uma associação anterior.
    udp.stop();
    udpStarted = false;
    WiFi.disconnect();
    WiFi.begin(ssid, psk, ioc_link_wifi_channel(node));
  }
  void leaveNode() override {
    joinedNode = 0;
    if (transport == Transport::Usb) { Serial.println("#IOC LEAVE"); return; }
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
  void render(const ioc_dl& dl) override {
    renderActive = true;
    memcpy(&renderDl, &dl, sizeof(renderDl));
    renderPending = true;
  }
  bool renderBusy() override { return renderActive; }
  void led(uint8_t level, bool on) override {
    static int last = -1;
    const int key = on ? level : 9;
    if (key == last) return;
    last = key;
    // Na bateria o LED só pisca nos agravamentos (beep); alimentado, mostra o nível.
    if (!on || (!onUsb && level < IOC_LEVEL_ATTENTION)) { M5.Led.setAllColor(0, 0, 0); M5.Led.display(); return; }
    static const uint8_t rgb[4][3] = {{0, 24, 0}, {0, 0, 60}, {80, 60, 0}, {120, 0, 0}};
    M5.Led.setAllColor(rgb[level & 3][0], rgb[level & 3][1], rgb[level & 3][2]);
    M5.Led.display();
  }
  void beep(uint8_t pattern) override {
    if (pattern == 1) { M5.Speaker.tone(2200, 120); delay(160); M5.Speaker.tone(2200, 120); }
    else if (pattern == 2) M5.Speaker.tone(3000, 30);
    else M5.Speaker.tone(880, 200);
  }
  void readSensors(uint8_t& pct, uint16_t& mv, int16_t& t, uint16_t& rh, bool& usb) override {
    refreshSensors(false);
    pct = batteryPct; mv = batteryMv; t = tempC10; rh = rh10; usb = onUsb || transport == Transport::Usb;
  }
  bool keyboardPresent() override { return keyboard; }
  void sleepFor(uint32_t seconds) override {
    while (renderActive) delay(50);
    stopRadios();
    Serial.flush();
    M5.Power.timerSleep(static_cast<int>(seconds));
  }
  void powerOff() override {
    while (renderActive) delay(50);
    stopRadios();
    Serial.flush();
    M5.Power.powerOff();
  }
  void storeU32(const char* key, uint32_t value) override {
    if (prefs.getUInt(key, value ^ 1u) != value) prefs.putUInt(key, value);
  }
  uint32_t loadU32(const char* key, uint32_t fallback) override { return prefs.getUInt(key, fallback); }
  void storeStr(const char* key, const char* value) override { prefs.putString(key, value); }
  bool loadStr(const char* key, char* out, size_t cap) override {
    const size_t n = prefs.getString(key, out, cap);
    return n > 0;
  }
  bool radioSurvey(char* out, size_t cap) override {
    if (transport == Transport::Usb) return false;
    size_t n = 0;
    const int found = WiFi.scanNetworks(false, true);
    int perChannel[14] = {0};
    n += snprintf(out + n, cap - n, "Pontos de acesso dos Nodes:\n");
    for (int i = 0; i < found && n < cap; ++i) {
      const int ch = WiFi.channel(i);
      if (ch > 0 && ch < 14) perChannel[ch]++;
      if (WiFi.SSID(i).startsWith("IOC-")) {
        n += snprintf(out + n, cap - n, "  %s canal %d %d dBm\n", WiFi.SSID(i).c_str(), ch, WiFi.RSSI(i));
      }
    }
    WiFi.scanDelete();
    n += snprintf(out + n, cap - n, "Redes por canal: 1=%d 6=%d 11=%d (de %d)\n", perChannel[1], perChannel[6],
                  perChannel[11], found < 0 ? 0 : found);
    n += snprintf(out + n, cap - n, "Anúncios BLE do projeto:\n");
    portENTER_CRITICAL(&radioMux);
    Seen copy[SEEN_MAX];
    memcpy(copy, seen, sizeof(copy));
    portEXIT_CRITICAL(&radioMux);
    for (const Seen& s : copy) {
      if (!s.uuid || ::millis() - s.ms > 30000 || n >= cap) continue;
      n += snprintf(out + n, cap - n, "  %08lX.. perfil %u %d dBm\n", static_cast<unsigned long>(s.uuid >> 32), s.profile, s.rssi);
    }
    if (joinedNode && WiFi.status() == WL_CONNECTED && n < cap) {
      snprintf(out + n, cap - n, "Associado ao IOC-%02u: %d dBm\n", joinedNode, WiFi.RSSI());
    }
    return true;
  }
  void log(const char* line) override { Serial.printf("#IOC LOG %s\n", line); }
} hal;

/* ---------------------------------------------------------- configuração */

void configureApp(Transport t) {
  transport = t;
  PanelConfig cfg;
  cfg.uuid = panelUuid;
  uint8_t master[IOC_LINK_KEY_LEN];
  haveNetworkKey = false;
  if (t == Transport::Usb) {
    ioc_link_hex_to_key(SIM_MASTER_KEY, master);
    ioc_link_derive_device_key(master, panelUuid, cfg.devKey);
    cfg.canSleep = false;
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
    cfg.canSleep = true;
  }
  app.begin(&hal, cfg);
}

void enterUsbMode() {
  if (transport == Transport::Usb) return;
  stopRadios();
  usbLink = false;
  configureApp(Transport::Usb);
  app.requestRefresh();
}

void handleHostLine(char* line) {
  if (strncmp(line, "#IOC ", 5) != 0) return;
  char* cmd = line + 5;
  static uint8_t buf[IOC_PANEL_DATAGRAM_MAX + IOC_ENV_OVERHEAD];
  if (strncmp(cmd, "BRIDGE", 6) == 0) {
    if (transport == Transport::Usb) {
      // A ponte pode ter sido encerrada com o aparelho ainda ligado.
      // READY deve iniciar uma sessão nova, não reaproveitar Bound.
      usbLink = false;
      app.restartLink();
    } else {
      enterUsbMode();
    }
    Serial.printf("#IOC READY uuid=0x%016llX profile=PANEL fw=%s\n", static_cast<unsigned long long>(panelUuid),
                  PANEL_FW_VERSION);
  } else if (strncmp(cmd, "TIME ", 5) == 0) {
    const int64_t epoch = atoll(cmd + 5);
    if (epoch > 1700000000LL && (!clockOk || llabs(epoch - hal.epochNow()) > 2)) hal.setClock(epoch);
  } else if (strncmp(cmd, "LINK ", 5) == 0) {
    usbLink = cmd[5] == '1';
  } else if (strncmp(cmd, "OFFER ", 6) == 0) {
    const size_t n = parseHex(cmd + 6, buf, sizeof(buf));
    app.onOffer(buf, n);
  } else if (strncmp(cmd, "DOWN ", 5) == 0) {
    const size_t n = parseHex(cmd + 5, buf, sizeof(buf));
    app.onDatagram(buf, n);
  } else if (strncmp(cmd, "KEY ", 4) == 0) {
    app.onKey(static_cast<uint8_t>(strtoul(cmd + 4, nullptr, 0)));
  } else if (strncmp(cmd, "BUTTON ", 7) == 0) {
    const char b = cmd[7];
    if (b == 'P') app.onPowerClick(cmd[8] == 'P');
    else app.onButton(b);
  }
}

void pollSerial() {
  static char line[2 * (IOC_PANEL_DATAGRAM_MAX + IOC_ENV_OVERHEAD) + 32];
  static size_t len = 0;
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len) { line[len] = '\0'; handleHostLine(line); len = 0; }
    } else if (len < sizeof(line) - 1) {
      line[len++] = static_cast<char>(c);
    } else {
      len = 0;  // linha longa demais: descarta
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
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.output_power = true;    // 5 V na porta HY2.0 para o CardKB
  cfg.internal_spk = true;
  M5.begin(cfg);
  // O firmware trata o botão de energia: sem reinício por um toque e sem
  // desligamento direto por dois toques no M5PM1 (o desligamento passa pela
  // página "Painel desligado" e pelo aviso ao Node).
  M5.In_I2C.bitOn(M5PM1_ADDR, 0x49, 0x01, I2C_FREQ);  // BTN_CFG_1.SINGLE_RST_DIS
  M5.In_I2C.bitOn(M5PM1_ADDR, 0x4A, 0x01, I2C_FREQ);  // BTN_CFG_2.DOUBLE_OFF_DIS
  M5.Power.setExtOutput(true);
  M5.Display.setRotation(0);
  M5.Display.setEpdMode(epd_mode_t::epd_fastest);
  M5.Display.setAutoDisplay(false);
  M5.Speaker.setVolume(96);
  prefs.begin("iocpanel", false);
  loadClock();
#if defined(IOT_PANEL_UUID)
  panelUuid = IOT_PANEL_UUID;
#else
  panelUuid = 0x5043000000000000ULL | (ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL);  // "PC" + MAC
#endif
  xTaskCreatePinnedToCore(renderTask, "papel", 8192, nullptr, 1, nullptr, 0);
  refreshSensors(true);
  probeKeyboard();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  startBle();
  configureApp(Transport::Wifi);
  esp_task_wdt_init(60, true);
  esp_task_wdt_add(nullptr);
  Serial.printf("#IOC HELLO uuid=0x%016llX profile=PANEL fw=%s keys=%s\n", static_cast<unsigned long long>(panelUuid),
                PANEL_FW_VERSION, haveNetworkKey ? "LINK" : "NONE");
}

void loop() {
  esp_task_wdt_reset();
  M5.update();
  if (M5.BtnPWR.wasClicked()) app.onPowerClick(M5.Power.M5pm1.wasPekDoubleClicked());
  if (M5.BtnA.wasClicked()) app.onButton('A');
  if (M5.BtnB.wasClicked()) app.onButton('B');
  if (M5.BtnC.wasClicked()) app.onButton('C');
  probeKeyboard();
  for (uint8_t k = readKey(), n = 0; k != 0 && n < 8; k = readKey(), ++n) app.onKey(k);
  pollSerial();
  pollRadio();
  app.loop();
  static uint32_t lastHello = 0;
  if (millis() - lastHello > (transport == Transport::Usb ? 10000u : 2000u)) {
    lastHello = millis();
    Serial.printf("#IOC HELLO uuid=0x%016llX profile=PANEL fw=%s mode=%s session=%u node=%u keyboard=%u\n",
                  static_cast<unsigned long long>(panelUuid), PANEL_FW_VERSION, transport == Transport::Usb ? "USB" : "WIFI",
                  app.sessionUp() ? 1u : 0u, app.parentNode(), keyboard ? 1u : 0u);
  }
  delay(5);
}
