/*
 * Ambiente de uma instância do firmware na bancada virtual: relógio, porta
 * serial, rádios e a ponte para SimInstance.
 *   -DSIM_NS=<namespace> -DSIM_FACTORY=<função> -DIOT_NODE_ID=<n>
 */
#include "sim_prelude.h"

namespace SIM_NS {
#include "sim_stubs.h"

#include "node_config.h"
#include "node_types.h"

/* Símbolos do firmware. */
extern ACAN2515 can;
extern NodeState state;
extern uint8_t leaderId;
void setup();
void loop();

/* Objetos que o núcleo Arduino forneceria. */
HostSerial Serial;
HostEsp ESP;
HostWiFi WiFi;
SPIClass SPI;
std::deque<HostDatagram> WiFiUDP::inbox;
std::deque<HostDatagram> WiFiUDP::outbox;

static uint64_t g_boot_us = 0;
static int g_station_count = 0;
static int g_station_rssi = 0;

uint32_t millis() { return static_cast<uint32_t>((::sim_now_us() - g_boot_us) / 1000u); }

// O relógio da bancada anda em passos; dentro de um passo, cada leitura de
// micros() avança um pouco, como no hardware. Sem isso, dois quadros
// retirados da fila na mesma passagem do laço teriam intervalo zero, o que
// esconderia os casos em que a Probe mede intervalos muito curtos.
uint32_t micros() {
  static uint64_t last_base = 0;
  static uint32_t reads = 0;
  const uint64_t base = ::sim_now_us() - g_boot_us;
  if (base != last_base) {
    last_base = base;
    reads = 0;
  }
  const uint32_t offset = reads < 12 ? reads * 37u : 444u;
  ++reads;
  return static_cast<uint32_t>(base + offset);
}
void delay(uint32_t ms) { ::sim_delay_ms(ms); }

int HostSerial::printf(const char* fmt, ...) {
  char buf[2048];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  captured += buf;
  return n;
}
void HostSerial::println(const char* text) {
  captured += text;
  captured += "\n";
}
void HostSerial::print(const char* text) { captured += text; }

void HostEsp::restart() { throw ::SimRestart{}; }

void esp_fill_random(void* buf, size_t len) {
  uint8_t* out = static_cast<uint8_t*>(buf);
  for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(::sim_random32() >> 24);
}

int esp_wifi_ap_get_sta_list(wifi_sta_list_t* list) {
  list->num = g_station_count;
  list->sta[0].rssi = static_cast<int8_t>(g_station_rssi);
  return ESP_OK;
}

class Instance : public ::SimInstance {
 public:
  uint8_t nodeId() const override { return NODE_ID; }

  void boot() override {
    g_boot_us = ::sim_now_us();
    setup();
  }
  void step() override { loop(); }

  bool canPeek(::SimCanFrame& out) const override {
    if (can.listenOnly || can.sent.empty()) return false;
    const CANMessage& msg = can.sent.front();
    out.id = msg.id;
    out.len = msg.len;
    memcpy(out.data, msg.data, 8);
    return true;
  }
  void canPop() override {
    if (can.sent.empty()) return;
    can.sent.erase(can.sent.begin());
    if (can.pendingInDriver > 0) --can.pendingInDriver;
  }
  void canDeliver(const ::SimCanFrame& frame) override {
    CANMessage msg;
    msg.id = frame.id;
    msg.len = frame.len;
    memcpy(msg.data, frame.data, 8);
    can.hostDeliver(msg);
  }
  bool canListenOnly() const override { return can.listenOnly; }
  void canSetErrorCounters(uint8_t rec, uint8_t tec) override {
    can.rec = rec;
    can.tec = tec;
  }

  std::string serialTake() override {
    std::string out;
    out.swap(Serial.captured);
    return out;
  }
  void serialWrite(const std::string& text) override { Serial.input += text; }

  void bleDeliver(const std::string& manufacturer, int rssi) override {
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan->scanning || !scan->callbacks) return;
    NimBLEAdvertisedDevice device;
    device.manufacturer = manufacturer;
    device.rssi = rssi;
    scan->callbacks->onResult(&device);
  }
  bool bleOffer(std::string& manufacturer) const override {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (!adv->advertising) return false;
    manufacturer = adv->manufacturer;
    return true;
  }

  bool apInfo(std::string& ssid, std::string& psk, int& channel) const override {
    if (!WiFi.apUp) return false;
    ssid = WiFi.ssid;
    psk = WiFi.psk;
    channel = WiFi.channel;
    return true;
  }
  void wifiStations(int count, int rssi) override {
    g_station_count = count;
    g_station_rssi = rssi;
  }
  void udpDeliver(const ::SimDatagram& datagram) override {
    HostDatagram d;
    d.data = datagram.data;
    d.ip = IPAddress(datagram.ip[0], datagram.ip[1], datagram.ip[2], datagram.ip[3]);
    d.port = datagram.port;
    WiFiUDP::inbox.push_back(d);
  }
  bool udpTake(::SimDatagram& datagram) override {
    if (WiFiUDP::outbox.empty()) return false;
    const HostDatagram& d = WiFiUDP::outbox.front();
    datagram.data = d.data;
    datagram.port = d.port;
    WiFiUDP::outbox.pop_front();
    return true;
  }

  int nodeState() const override { return static_cast<int>(state); }
  uint8_t leaderId() const override { return SIM_NS::leaderId; }
};

}  // namespace SIM_NS

::SimInstance* SIM_FACTORY() { return new SIM_NS::Instance(); }
