// Exercita o MESMO PanelApp do firmware: retransmissão de HELLO,
// desafio HMAC real, perda de sessão e nova autenticação no mesmo Node.
#include "panel_app.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

class MockHal final : public PanelHal {
 public:
  uint32_t now = 1000;
  int joins = 0;
  int leaves = 0;
  uint8_t joined = 0;
  bool connected = true;
  std::map<std::string, uint32_t> persisted{{"node", 2}};
  std::vector<std::vector<uint8_t>> out;

  uint32_t millis() override { return now; }
  int64_t epochNow() override { return 1760000000 + now / 1000; }
  bool clockValid() override { return true; }
  void setClock(int64_t) override {}
  void joinNode(uint8_t node) override { joined = node; ++joins; connected = true; }
  void leaveNode() override { joined = 0; ++leaves; connected = false; }
  bool linkUp() override { return connected; }
  bool sendDatagram(const uint8_t* data, size_t len) override {
    out.emplace_back(data, data + len);
    return true;
  }
  void render(const ioc_dl&) override {}
  bool renderBusy() override { return false; }
  void led(uint8_t, bool) override {}
  void beep(uint8_t) override {}
  void readSensors(uint8_t& pct, uint16_t& mv, int16_t& t, uint16_t& rh, bool& usb) override {
    pct = 90; mv = 3900; t = 230; rh = 450; usb = true;
  }
  bool keyboardPresent() override { return false; }
  void sleepFor(uint32_t) override {}
  void powerOff() override {}
  void storeU32(const char* key, uint32_t value) override { persisted[key] = value; }
  uint32_t loadU32(const char* key, uint32_t fallback) override {
    const auto it = persisted.find(key);
    return it == persisted.end() ? fallback : it->second;
  }
  void storeStr(const char*, const char*) override {}
  bool loadStr(const char*, char*, size_t) override { return false; }
  bool radioSurvey(char*, size_t) override { return false; }
  void log(const char*) override {}
};

static std::vector<uint8_t> lastHello(const MockHal& hal) {
  for (auto it = hal.out.rbegin(); it != hal.out.rend(); ++it) {
    ioc_env_view view;
    if (ioc_env_parse(it->data(), it->size(), &view) && view.type == IOC_MSG_HELLO) {
      return *it;
    }
  }
  assert(false && "HELLO não emitido");
  return {};
}

static ioc_hello_t decodeHello(const std::vector<uint8_t>& datagram) {
  ioc_env_view env;
  assert(ioc_env_parse(datagram.data(), datagram.size(), &env));
  assert(env.type == IOC_MSG_HELLO);
  ioc_hello_t hello{};
  assert(ioc_hello_decode(env.payload, env.payload_len, &hello));
  return hello;
}

int main() {
  MockHal hal;
  PanelConfig config{};
  config.uuid = 0x504300000000BEEFULL;
  for (int i = 0; i < IOC_LINK_KEY_LEN; ++i) config.devKey[i] = static_cast<uint8_t>(i + 1);
  config.canSleep = false;

  PanelApp panel;
  panel.begin(&hal, config);
  assert(hal.joins == 1);
  assert(hal.joined == 2);

  panel.loop();
  const auto first = lastHello(hal);
  assert(ioc_env_verify(config.devKey, first.data(), first.size()));

  // CHALLENGE perdido: retransmissões precisam conservar o MESMO nonce.
  const size_t sent = hal.out.size();
  hal.now += 7000;
  panel.loop();
  assert(hal.out.size() > sent);
  const auto second = lastHello(hal);
  assert(first == second);

  // Emite um CHALLENGE autenticado e confirma que o painel se torna Bound.
  const ioc_hello_t hello = decodeHello(first);
  ioc_challenge_t ch{};
  ch.node_id = 2;
  ch.child_id = 1;
  memcpy(ch.sensor_nonce, hello.sensor_nonce, IOC_LINK_NONCE_LEN);
  for (int i = 0; i < IOC_LINK_NONCE_LEN; ++i) ch.node_nonce[i] = static_cast<uint8_t>(0xA0 + i);
  uint8_t raw[IOC_CHALLENGE_LEN], packet[IOC_CHALLENGE_LEN + IOC_ENV_OVERHEAD];
  ioc_challenge_encode(&ch, raw);
  const size_t n = ioc_env_seal(config.devKey, IOC_MSG_CHALLENGE, 0,
                                 raw, sizeof(raw), packet, sizeof(packet));
  assert(n > 0);
  panel.onDatagram(packet, n);
  assert(panel.sessionUp());
  assert(panel.parentNode() == 2);

  // Ao perder PING/PONG por mais de 10s, deve abrir de novo o MESMO Node.
  const int before = hal.joins;
  hal.now += 11000;
  panel.loop();
  assert(!panel.sessionUp());
  assert(hal.joins == before + 1);
  assert(hal.joined == 2);
  panel.loop();
  const auto third = lastHello(hal);
  assert(third != first);

  // Reabertura da ponte USB com a sessão ainda viva: o BRIDGE chama
  // restartLink() e obriga um novo JOIN, sem reutilizar chave/contadores.
  const ioc_hello_t hello2 = decodeHello(third);
  memcpy(ch.sensor_nonce, hello2.sensor_nonce, IOC_LINK_NONCE_LEN);
  ioc_challenge_encode(&ch, raw);
  const size_t n2 = ioc_env_seal(config.devKey, IOC_MSG_CHALLENGE, 0,
                                  raw, sizeof(raw), packet, sizeof(packet));
  panel.onDatagram(packet, n2);
  assert(panel.sessionUp());
  const int lastJoins = hal.joins;
  panel.restartLink();
  assert(!panel.sessionUp());
  assert(hal.joins == lastJoins + 1);
  panel.loop();
  assert(lastHello(hal) != third);
  return 0;
}
