/*
 * Bancada virtual: a Probe 00, três Nodes CAN e sensores wireless simulados
 * em um único processo.
 *
 * Cada Node é o firmware real (src/main.ino e demais arquivos), compilado em
 * um namespace próprio (sim_unit.cpp, sim_instance.cpp). Esta bancada fornece
 * o que no laboratório é físico: o barramento CAN com arbitragem e duração
 * real dos quadros, o rádio BLE, o ponto de acesso Wi-Fi, o relógio e os
 * sensores. Os sensores seguem as mesmas regras do firmware do Pico W
 * (Codigo/node-wifi/edge_network_driver.c) e usam a biblioteca ioc_link.
 *
 * Uso: sim_network <cenário> [--verbose]
 *
 * A saída padrão traz as linhas que a Probe 00 enviaria à TUI, cada uma
 * precedida do instante simulado ("@12345 [GW] ..."). Linhas iniciadas por
 * "#" são anotações da bancada. Com --verbose, a serial dos Nodes também é
 * impressa ("#N3@12345 ..."). Uma verificação que falhe encerra com código 1.
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sim_prelude.h"

#ifndef IOT_LINK_MASTER_KEY_HEX
#error "IOT_LINK_MASTER_KEY_HEX precisa ser definido (chave de teste)"
#endif

/* Instâncias do firmware. Um reinício consome uma instância: o estado em RAM
 * de quem reiniciou precisa voltar ao inicial, e isso só existe em outra
 * cópia das variáveis globais. */
::SimInstance* sim_make_probe();
::SimInstance* sim_make_n1a();
::SimInstance* sim_make_n1b();
::SimInstance* sim_make_n2a();
::SimInstance* sim_make_n2b();
::SimInstance* sim_make_n3a();
::SimInstance* sim_make_n3b();

namespace {

/* ------------------------------------------------------------ relógio */

constexpr uint32_t TICK_US = 500;
constexpr uint32_t CAN_BIT_US = 2;          // 500 kbit/s
constexpr uint32_t BOOT_DELAY_MS = 1200;    // tempo até o firmware voltar a executar após o reinício

uint64_t g_now_us = 1000;
uint32_t g_rng = 0x1234567u;
bool g_verbose = false;
int g_failures = 0;

uint32_t nowMs() { return static_cast<uint32_t>(g_now_us / 1000u); }

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      fprintf(stderr, "FALHA t=%u ms %s:%d: %s\n", nowMs(), __FILE__, __LINE__, #cond); \
      g_failures++;                                                              \
    }                                                                            \
  } while (0)

void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  printf("# t=%u %s\n", nowMs(), buf);
}

/* --------------------------------------------------------------- Nodes */

enum SimNodeState {  // mesmos valores de node_types.h
  ST_IDLE, ST_GATEWAY, ST_JOINING, ST_ELECTION, ST_RECOVERING, ST_LEADER, ST_FOLLOWER, ST_FAULT
};

struct Node {
  uint8_t id = 0;
  std::vector<std::function<::SimInstance*()>> generations;
  size_t nextGeneration = 0;
  ::SimInstance* inst = nullptr;
  bool powered = false;
  bool bootScheduled = false;
  uint64_t bootAtUs = 0;
  bool txDisconnected = false;   // o que este nó transmite não chega ao barramento
  std::string partial;
  std::vector<std::pair<uint32_t, std::string>> log;
  uint8_t tec = 0;
  uint32_t restarts = 0;
};

std::vector<Node> g_nodes;
uint64_t g_bus_free_us = 0;
uint64_t g_bus_bits = 0;
uint32_t g_bus_frames = 0;

Node& node(uint8_t id) {
  for (Node& n : g_nodes) if (n.id == id) return n;
  fprintf(stderr, "Node %u inexistente\n", id);
  exit(2);
}

Node& probe() { return node(0); }

bool logHas(const Node& n, const char* text, uint32_t sinceMs = 0) {
  for (const auto& entry : n.log) {
    if (entry.first >= sinceMs && entry.second.find(text) != std::string::npos) return true;
  }
  return false;
}

/* Instante da primeira linha que contém o texto, a partir de sinceMs; -1 se não houver. */
long logTime(const Node& n, const char* text, uint32_t sinceMs = 0) {
  for (const auto& entry : n.log) {
    if (entry.first >= sinceMs && entry.second.find(text) != std::string::npos) {
      return static_cast<long>(entry.first);
    }
  }
  return -1;
}

int logCount(const Node& n, const char* text, uint32_t sinceMs = 0) {
  int count = 0;
  for (const auto& entry : n.log) {
    if (entry.first >= sinceMs && entry.second.find(text) != std::string::npos) ++count;
  }
  return count;
}

const std::string* logLast(const Node& n, const char* text) {
  for (auto it = n.log.rbegin(); it != n.log.rend(); ++it) {
    if (it->second.find(text) != std::string::npos) return &it->second;
  }
  return nullptr;
}

/* Valor numérico de "chave=" em uma linha; NAN se ausente. */
double field(const std::string& line, const char* key) {
  const std::string needle = std::string(" ") + key + "=";
  size_t pos = line.find(needle);
  if (pos == std::string::npos) return NAN;
  return strtod(line.c_str() + pos + needle.size(), nullptr);
}

void collectSerial(Node& n) {
  if (!n.inst) return;
  n.partial += n.inst->serialTake();
  size_t start = 0;
  for (;;) {
    const size_t end = n.partial.find('\n', start);
    if (end == std::string::npos) break;
    std::string line = n.partial.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (n.id == 0) {
      printf("@%u %s\n", nowMs(), line.c_str());
    } else if (g_verbose) {
      printf("#N%u@%u %s\n", n.id, nowMs(), line.c_str());
    }
    n.log.emplace_back(nowMs(), std::move(line));
  }
  n.partial.erase(0, start);
}

void powerOn(Node& n) {
  if (n.nextGeneration >= n.generations.size()) {
    fprintf(stderr, "Node %u: nao ha mais instancias para reiniciar\n", n.id);
    exit(2);
  }
  n.inst = n.generations[n.nextGeneration++]();
  n.powered = true;
  n.bootScheduled = false;
  n.tec = 0;
  n.inst->boot();
  collectSerial(n);
  if (n.id != 0) note("node=%u ligado", n.id);
}

void powerOff(Node& n, bool rebootAfter) {
  collectSerial(n);
  n.powered = false;
  n.inst = nullptr;  // a instância não é reutilizada
  if (rebootAfter) {
    n.bootScheduled = true;
    n.bootAtUs = g_now_us + static_cast<uint64_t>(BOOT_DELAY_MS) * 1000u;
    ++n.restarts;
  }
}

/* ---------------------------------------------------------- barramento */

// Transmite os quadros que cabem até `untilUs`: vence o menor identificador;
// sem outro nó para confirmar, o quadro permanece na fila de quem transmite.
void pumpBus(uint64_t untilUs) {
  while (g_bus_free_us < untilUs) {
    Node* winner = nullptr;
    ::SimCanFrame best;
    for (Node& n : g_nodes) {
      if (!n.powered || !n.inst) continue;
      ::SimCanFrame frame;
      if (!n.inst->canPeek(frame)) continue;
      if (n.txDisconnected) {  // o quadro deixa o nó e se perde
        n.inst->canPop();
        continue;
      }
      if (!winner || frame.id < best.id) {
        winner = &n;
        best = frame;
      }
    }
    if (!winner) {
      g_bus_free_us = untilUs;
      return;
    }

    const uint32_t bits = ioc_can_frame_bits(static_cast<uint16_t>(best.id), best.len, best.data);
    const uint64_t start = std::max(g_bus_free_us, g_now_us);
    g_bus_free_us = start + static_cast<uint64_t>(bits) * CAN_BIT_US;

    bool acked = false;
    for (Node& n : g_nodes) {
      if (&n == winner || !n.powered || !n.inst) continue;
      if (!n.inst->canListenOnly()) acked = true;
    }
    if (!acked) {
      winner->tec = winner->tec > 120 ? 128 : static_cast<uint8_t>(winner->tec + 8);
      winner->inst->canSetErrorCounters(0, winner->tec);
      continue;  // retransmissão
    }
    if (winner->tec > 0) {
      --winner->tec;
      winner->inst->canSetErrorCounters(0, winner->tec);
    }
    winner->inst->canPop();
    g_bus_bits += bits;
    ++g_bus_frames;
    for (Node& n : g_nodes) {
      if (&n == winner || !n.powered || !n.inst) continue;
      n.inst->canDeliver(best);
    }
  }
}

/* ------------------------------------------------------------- sensores */

uint8_t g_master[IOC_LINK_KEY_LEN];
uint8_t g_network_key[IOC_LINK_KEY_LEN];

constexpr uint32_t BEACON_PERIOD_MS = 500;
constexpr uint32_t OFFER_SCAN_MS = 300;
constexpr uint32_t WIFI_JOIN_MS = 1500;

struct Sensor {
  enum class Mode { Idle, Discovery, Bound };

  uint64_t uuid = 0;
  uint8_t ip[4] = {192, 168, 4, 2};
  uint16_t port = 4242;
  uint8_t devKey[IOC_LINK_KEY_LEN] = {0};
  std::map<uint8_t, int> rssi;   // nós que o alcançam (BLE e Wi-Fi)
  bool poweredOn = true;
  // Como o Pico W: uma amostra por janela processada (512 amostras a 1 kHz).
  // O Node encaminha ao CAN uma por segundo.
  uint32_t telemetryPeriodMs = 500;

  Mode mode = Mode::Idle;
  uint8_t parent = 0;            // Node em cujo ponto de acesso está (0 = nenhum)
  uint8_t child = 0;
  uint64_t wifiReadyUs = 0;
  uint64_t discoverySinceUs = 0;
  uint64_t lastOfferUs = 0;
  uint64_t lastActivityUs = 0;
  uint64_t nextHelloUs = 0;
  uint64_t nextBeaconUs = 0;
  uint64_t nextOfferScanUs = 0;
  uint64_t nextTelemetryUs = 0;
  uint8_t sessionKey[IOC_LINK_KEY_LEN] = {0};
  uint8_t nonce[IOC_LINK_NONCE_LEN] = {0};
  uint32_t tx = 0;
  uint32_t rxLast = 0;
  uint16_t seq = 0;
  uint32_t sessions = 0;
  uint32_t boundAtMs = 0;

  bool inRange(uint8_t nodeId) const { return rssi.count(nodeId) > 0; }

  bool linkUp() const {
    if (parent == 0 || g_now_us < wifiReadyUs || !inRange(parent)) return false;
    Node& n = node(parent);
    if (!n.powered || !n.inst) return false;
    std::string ssid, psk;
    int channel = 0;
    if (!n.inst->apInfo(ssid, psk, channel)) return false;
    char expectSsid[IOC_LINK_SSID_LEN];
    char expectPsk[IOC_LINK_PSK_LEN];
    ioc_link_wifi_ssid(parent, expectSsid);
    ioc_link_wifi_psk(g_network_key, parent, expectPsk);
    return ssid == expectSsid && psk == expectPsk && channel == ioc_link_wifi_channel(parent);
  }

  void send(const uint8_t* key, uint8_t type, uint32_t counter, const void* payload, size_t len) {
    if (!linkUp()) return;
    ::SimDatagram d;
    d.data.resize(len + IOC_ENV_OVERHEAD);
    const size_t n = ioc_env_seal(key, type, counter, static_cast<const uint8_t*>(payload), len,
                                  d.data.data(), d.data.size());
    CHECK(n == d.data.size());
    memcpy(d.ip, ip, 4);
    d.port = port;
    node(parent).inst->udpDeliver(d);
  }

  void sendData(const void* inner, size_t len) {
    if (tx == 0xFFFFFFFFu) return;
    send(sessionKey, IOC_MSG_DATA_UP, ++tx, inner, len);
  }

  void hello() {
    for (int i = 0; i < IOC_LINK_NONCE_LEN; ++i) nonce[i] = static_cast<uint8_t>(sim_random32() >> 24);
    ioc_hello_t h;
    h.uuid = uuid;
    h.profile = PROFILE_ACCELEROMETER;
    h.protocol = NET_PROTOCOL_VERSION;
    memcpy(h.sensor_nonce, nonce, IOC_LINK_NONCE_LEN);
    uint8_t raw[IOC_HELLO_LEN];
    ioc_hello_encode(&h, raw);
    send(devKey, IOC_MSG_HELLO, 0, raw, sizeof(raw));
  }

  void configMenu() {
    Payload_ConfigMenu menu{};
    menu.magic_header = NET_MAGIC_HEADER;
    menu.cmd_type = CMD_CONFIG_MENU;
    menu.current_buffer_size = 512;
    menu.current_fsm_mode = FSM_MODE_ROTATING_MACH;
    menu.current_window_type = 1;
    menu.current_sample_rate_hz = 1000.0f;
    menu.current_stalta_thresh = 4.0f;
    menu.current_calib_gain = 1.0f;
    sendData(&menu, sizeof(menu));
  }

  void telemetry() {
    Payload_TelemetryStream t{};
    t.magic_header = NET_MAGIC_HEADER;
    t.cmd_type = CMD_TELEMETRY_STREAM;
    t.seq_num = seq++;
    t.fsm_mode = FSM_MODE_ROTATING_MACH;
    t.acquisition_mode = ACQ_MODE_POLLING;
    t.axis_mask = AXIS_MASK_VECTOR;
    t.fft_valid = 0;
    t.window_size = 512;
    t.sample_rate_req_hz = 1000.0f;
    t.sample_rate_eff_hz = 1000.0f;
    t.rms_ac = 0.12f + 0.01f * static_cast<float>(seq % 5);
    t.kurtosis = -0.2f;
    t.crest_factor = 2.8f;
    t.peak_freq_hz = 31.25f;
    t.peak_amplitude = 0.02f;
    t.entropy = 0.45f;
    t.ppv_max_mm_s = 0.4f;
    t.battery_pct = 93;
    t.battery_mv = 3980;
    sendData(&t, sizeof(t));
  }

  void enterDiscovery() {
    mode = Mode::Discovery;
    discoverySinceUs = g_now_us;
    nextHelloUs = g_now_us;
    memset(sessionKey, 0, sizeof(sessionKey));
  }

  void joinAccessPoint(uint8_t nodeId) {
    parent = nodeId;
    wifiReadyUs = g_now_us + static_cast<uint64_t>(WIFI_JOIN_MS) * 1000u;
    enterDiscovery();
    note("sensor=0x%016llX entrando no ponto de acesso do node=%u", static_cast<unsigned long long>(uuid),
         nodeId);
  }

  void onOffer(uint8_t nodeId) {
    lastOfferUs = g_now_us;
    if (mode == Mode::Bound) return;  // com sessão, ofertas são ignoradas
    const bool idle = parent == 0;
    const bool maySwitch = nodeId != parent &&
                           g_now_us - discoverySinceUs > static_cast<uint64_t>(NET_OFFER_SWITCH_HOLD_MS) * 1000u;
    if (idle || maySwitch) joinAccessPoint(nodeId);
  }

  void onDatagram(const ::SimDatagram& d) {
    ioc_env_view view;
    if (!ioc_env_parse(d.data.data(), d.data.size(), &view)) return;

    if (view.type == IOC_MSG_CHALLENGE) {
      if (mode != Mode::Discovery) return;
      if (!ioc_env_verify(devKey, d.data.data(), d.data.size())) return;
      ioc_challenge_t ch;
      if (!ioc_challenge_decode(view.payload, view.payload_len, &ch)) return;
      if (memcmp(ch.sensor_nonce, nonce, IOC_LINK_NONCE_LEN) != 0) return;
      ioc_link_derive_session_key(devKey, uuid, ch.node_id, nonce, ch.node_nonce, sessionKey);
      child = ch.child_id;
      mode = Mode::Bound;
      tx = 0;
      rxLast = 0;
      lastActivityUs = g_now_us;
      nextTelemetryUs = g_now_us + 200000u;
      boundAtMs = nowMs();
      ++sessions;
      ioc_confirm_t cf;
      memcpy(cf.node_nonce, ch.node_nonce, IOC_LINK_NONCE_LEN);
      memcpy(cf.sensor_nonce, nonce, IOC_LINK_NONCE_LEN);
      uint8_t raw[IOC_CONFIRM_LEN];
      ioc_confirm_encode(&cf, raw);
      send(sessionKey, IOC_MSG_CONFIRM, ++tx, raw, sizeof(raw));
      configMenu();
      note("sensor=0x%016llX sessao com node=%u filho=%u", static_cast<unsigned long long>(uuid), parent,
           child);
      return;
    }

    if (view.type != IOC_MSG_DATA_DOWN || mode != Mode::Bound) return;
    if (!ioc_env_verify(sessionKey, d.data.data(), d.data.size())) return;
    if (!ioc_counter_accept(&rxLast, view.counter)) return;
    lastActivityUs = g_now_us;
    if (view.payload_len < 3) return;
    switch (view.payload[2]) {
      case CMD_PING: {
        Payload_Heartbeat ping{};
        if (view.payload_len >= sizeof(ping)) memcpy(&ping, view.payload, sizeof(ping));
        Payload_Heartbeat pong{};
        pong.magic_header = NET_MAGIC_HEADER;
        pong.cmd_type = CMD_PONG;
        pong.request_counter = ping.request_counter;
        sendData(&pong, sizeof(pong));
        break;
      }
      case CMD_GET_CONFIG_MENU:
        configMenu();
        break;
      case CMD_RELEASE:
        note("sensor=0x%016llX liberado pelo node=%u", static_cast<unsigned long long>(uuid), parent);
        mode = Mode::Idle;
        parent = 0;
        lastOfferUs = 0;
        break;
      default:
        break;
    }
  }

  void step() {
    if (!poweredOn) return;

    // Anúncio de presença, ouvido pelos Nodes ao alcance.
    if (g_now_us >= nextBeaconUs) {
      nextBeaconUs = g_now_us + static_cast<uint64_t>(BEACON_PERIOD_MS + sim_random32() % 40u) * 1000u;
      std::string mfg(15, '\0');
      mfg[0] = static_cast<char>(0xFF);
      mfg[1] = static_cast<char>(0xFF);
      mfg[2] = 'I';
      mfg[3] = 'C';
      mfg[4] = 0x01;
      mfg[5] = static_cast<char>(PROFILE_ACCELEROMETER);
      mfg[6] = static_cast<char>(NET_PROTOCOL_VERSION);
      for (int i = 0; i < 8; ++i) mfg[7 + i] = static_cast<char>((uuid >> (56 - 8 * i)) & 0xFFu);
      for (const auto& entry : rssi) {
        Node& n = node(entry.first);
        if (n.powered && n.inst) n.inst->bleDeliver(mfg, entry.second);
      }
    }

    // Ofertas de vínculo anunciadas pelos Nodes.
    if (mode != Mode::Bound && g_now_us >= nextOfferScanUs) {
      nextOfferScanUs = g_now_us + static_cast<uint64_t>(OFFER_SCAN_MS) * 1000u;
      for (const auto& entry : rssi) {
        Node& n = node(entry.first);
        if (!n.powered || !n.inst) continue;
        std::string mfg;
        if (!n.inst->bleOffer(mfg)) continue;
        ioc_offer_t offer;
        if (!ioc_offer_verify(devKey, reinterpret_cast<const uint8_t*>(mfg.data()), mfg.size(), &offer)) continue;
        if (offer.uuid != uuid || offer.node_id != entry.first) continue;
        onOffer(offer.node_id);
      }
    }

    if (mode == Mode::Bound) {
      if (g_now_us - lastActivityUs > static_cast<uint64_t>(NET_SESSION_TIMEOUT_MS) * 1000u) {
        note("sensor=0x%016llX sessao expirada com node=%u", static_cast<unsigned long long>(uuid), parent);
        enterDiscovery();
      } else if (g_now_us >= nextTelemetryUs) {
        nextTelemetryUs = g_now_us + static_cast<uint64_t>(telemetryPeriodMs) * 1000u;
        telemetry();
      }
    } else if (mode == Mode::Discovery) {
      if (linkUp() && g_now_us >= nextHelloUs) {
        nextHelloUs = g_now_us + static_cast<uint64_t>(1500u + sim_random32() % 2001u) * 1000u;
        hello();
      }
      const uint64_t giveUp = static_cast<uint64_t>(NET_DISCOVERY_GIVEUP_MS) * 1000u;
      if (g_now_us - discoverySinceUs > giveUp && g_now_us - lastOfferUs > giveUp / 3u) {
        mode = Mode::Idle;
        parent = 0;
      }
    }
  }
};

std::vector<Sensor> g_sensors;

Sensor& addSensor(uint64_t uuid, uint16_t port, uint8_t lastOctet) {
  Sensor s;
  s.uuid = uuid;
  s.port = port;
  s.ip[3] = lastOctet;
  ioc_link_derive_device_key(g_master, uuid, s.devKey);
  g_sensors.push_back(s);
  return g_sensors.back();
}

void routeWifi() {
  for (Node& n : g_nodes) {
    if (n.id == 0 || !n.powered || !n.inst) continue;
    int stations = 0;
    int stationRssi = 0;
    for (const Sensor& s : g_sensors) {
      if (s.poweredOn && s.parent == n.id && s.linkUp()) {
        ++stations;
        stationRssi = s.rssi.at(n.id);
      }
    }
    n.inst->wifiStations(stations, stationRssi);
    ::SimDatagram d;
    while (n.inst->udpTake(d)) {
      for (Sensor& s : g_sensors) {
        if (s.poweredOn && s.parent == n.id && s.port == d.port && s.linkUp()) s.onDatagram(d);
      }
    }
  }
}

/* ------------------------------------------------------------- execução */

void tick() {
  for (Node& n : g_nodes) {
    if (n.bootScheduled && g_now_us >= n.bootAtUs) powerOn(n);
  }
  for (Sensor& s : g_sensors) s.step();
  for (Node& n : g_nodes) {
    if (!n.powered || !n.inst) continue;
    try {
      n.inst->step();
    } catch (const ::SimRestart&) {
      note("node=%u reiniciando", n.id);
      powerOff(n, true);
      continue;
    }
    collectSerial(n);
  }
  routeWifi();
  pumpBus(g_now_us + TICK_US);
  g_now_us += TICK_US;
}

void runUntil(uint32_t ms) {
  while (nowMs() < ms) tick();
}

void runFor(uint32_t ms) { runUntil(nowMs() + ms); }

/* Executa até a condição valer ou o prazo vencer; devolve se valeu. */
bool runUntilTrue(const std::function<bool()>& condition, uint32_t timeoutMs) {
  const uint32_t deadline = nowMs() + timeoutMs;
  while (nowMs() < deadline) {
    if (condition()) return true;
    tick();
  }
  return condition();
}

void probeCommand(const std::string& line) {
  note("comando: %s", line.c_str());
  probe().inst->serialWrite(line + "\n");
}

// Um Node fora do ar por um ensaio mantém o estado que tinha; não conta.
bool active(const Node& n) {
  return n.id != 0 && n.powered && n.inst && !n.inst->canListenOnly();
}

int leaderCount() {
  int count = 0;
  for (Node& n : g_nodes) {
    if (active(n) && n.inst->nodeState() == ST_LEADER) ++count;
  }
  return count;
}

uint8_t currentLeader() {
  for (Node& n : g_nodes) {
    if (active(n) && n.inst->nodeState() == ST_LEADER) return n.id;
  }
  return 0;
}

/* ------------------------------------------------------------- cenários */

const uint64_t SENSOR_A = 0xE6616408432B6F39ull;
const uint64_t SENSOR_B = 0xE66164084327A12Cull;

void setupBench() {
  g_sensors.reserve(8);
  Node p;
  p.id = 0;
  p.generations = {sim_make_probe};
  g_nodes.push_back(p);
  Node n1;
  n1.id = 1;
  n1.generations = {sim_make_n1a, sim_make_n1b};
  g_nodes.push_back(n1);
  Node n2;
  n2.id = 2;
  n2.generations = {sim_make_n2a, sim_make_n2b};
  g_nodes.push_back(n2);
  Node n3;
  n3.id = 3;
  n3.generations = {sim_make_n3a, sim_make_n3b};
  g_nodes.push_back(n3);
  for (Node& n : g_nodes) powerOn(n);
}

// Rede formada: eleição iniciada pela Probe, Node 3 líder, heartbeat em curso.
void formNetwork() {
  runUntil(1000);
  probeCommand("22 00 FF 01");
  CHECK(runUntilTrue([] { return logHas(probe(), "[GW] HEARTBEAT lider=3"); }, 12000));
  CHECK(currentLeader() == 3 && leaderCount() == 1);
  CHECK(node(1).inst->nodeState() == ST_FOLLOWER && node(2).inst->nodeState() == ST_FOLLOWER);
}

// Sensor A associado ao Node 1, com sessão de dados e telemetria chegando à Probe.
Sensor& bindSensorA(bool automatic) {
  Sensor& a = addSensor(SENSOR_A, 4242, 2);
  a.rssi[1] = -50;
  a.rssi[2] = -68;
  CHECK(runUntilTrue([] { return logHas(probe(), "WIRELESS_CANDIDATE reporter=1 uuid=0xE6616408432B6F39") &&
                                 logHas(probe(), "WIRELESS_CANDIDATE reporter=2 uuid=0xE6616408432B6F39"); },
                     8000));
  probeCommand(std::string("WIRELESS BIND 1 0xE6616408432B6F39") + (automatic ? " AUTO" : ""));
  CHECK(runUntilTrue([] { return logHas(probe(), "TEL NODE=1 CHILD=1 "); }, 15000));
  CHECK(logHas(probe(), "[GW] WIRELESS_LINK node=1 child=1 state=SECURE"));
  return g_sensors.back();
}

int scenarioFormation() {
  formNetwork();
  runFor(12000);
  // Uma linha de estatística por segundo, com ocupação medida a partir dos quadros.
  CHECK(logCount(probe(), "[GW] BUS_STATS ") >= 15);
  const std::string* stats = logLast(probe(), "[GW] BUS_STATS ");
  CHECK(stats != nullptr);
  if (stats) {
    CHECK(field(*stats, "frames") >= 2 && field(*stats, "load") > 0.0 && field(*stats, "load") < 5.0);
    CHECK(field(*stats, "peak") >= field(*stats, "load"));
    CHECK(field(*stats, "rx_err") == 0 && field(*stats, "tx_err") == 0);
  }
  // Período do heartbeat medido pela Probe: 1 s, com variação da ordem do laço.
  const std::string* hb = logLast(probe(), "[GW] ID_STATS id=0x100 ");
  CHECK(hb != nullptr);
  if (hb) {
    CHECK(field(*hb, "n") >= 4 && field(*hb, "dlc") == 5);
    CHECK(std::fabs(field(*hb, "dt_avg_us") - 1000000.0) < 3000.0);
    CHECK(field(*hb, "dt_max_us") - field(*hb, "dt_min_us") < 6000.0);
  }
  CHECK(logHas(probe(), "[GW] ID_STATS id=0x200 "));  // sensores locais no ciclo TDMA
  CHECK(logHas(probe(), "[GW] SENSOR sensor=1 ") && logHas(probe(), "[GW] SENSOR sensor=2 "));

  probeCommand("METRICS OFF");
  runFor(200);
  const int before = logCount(probe(), "[GW] BUS_STATS ");
  runFor(3000);
  CHECK(logCount(probe(), "[GW] BUS_STATS ") == before);
  probeCommand("METRICS ON");
  runFor(2500);
  CHECK(logCount(probe(), "[GW] BUS_STATS ") >= before + 2);
  return 0;
}

int scenarioSensor() {
  formNetwork();
  bindSensorA(true);
  runFor(15000);
  CHECK(logHas(probe(), "WIRELESS_ASSOC node=1 child=1 uuid=0xE6616408432B6F39 profile=VIBRATION state=ONLINE") );
  CHECK(logHas(probe(), "failover=AUTO session=SECURE"));
  // Tempo de ida e volta Node <-> sensor publicado junto do estado do enlace.
  const std::string* link = logLast(probe(), "[GW] WIRELESS_LINK node=1 child=1 state=SECURE");
  CHECK(link != nullptr);
  if (link) CHECK(field(*link, "rtt_ms") >= 0.0 && field(*link, "rtt_ms") < 20.0 && field(*link, "rtt_max_ms") >= field(*link, "rtt_ms"));
  CHECK(logCount(probe(), "TEL NODE=1 CHILD=1 ") >= 12);

  // Tempo de resposta de um comando, medido pela Probe.
  probeCommand("CMD TARGET=01.01 ACTION=STATUS TX=B00001");
  CHECK(runUntilTrue([] { return logHas(probe(), "ACK COMMAND=STATUS STATE=APPLIED TX=B00001"); }, 2000));
  const std::string* ack = logLast(probe(), "ACK COMMAND=STATUS STATE=APPLIED TX=B00001");
  if (ack) CHECK(field(*ack, "RTT_MS") >= 0.0 && field(*ack, "RTT_MS") < 200.0);

  // Mudança de política sem refazer o vínculo.
  probeCommand("WIRELESS POLICY 1 0xE6616408432B6F39 MANUAL");
  CHECK(runUntilTrue([] { return logHas(probe(), "state=ONLINE rssi=-50 protocol=6 failover=MANUAL session=SECURE"); },
                     3000));
  probeCommand("WIRELESS POLICY 1 0xE6616408432B6F39 AUTO");
  const uint32_t t0 = nowMs();
  CHECK(runUntilTrue([t0] { return logHas(probe(), "failover=AUTO session=SECURE", t0); }, 3000));
  CHECK(!logHas(probe(), "[GW] WIRELESS_FAILOVER"));

  // As transferências segmentadas entram nas estatísticas.
  const std::string* stats = logLast(probe(), "[GW] BUS_STATS ");
  if (stats) CHECK(field(*stats, "xfer_ok") >= 20 && field(*stats, "xfer_err") == 0);
  CHECK(logHas(probe(), "[GW] ID_STATS id=0x381 "));
  return 0;
}

// O Node responsável pelo sensor sai do ar; o líder entrega o sensor a outro.
int scenarioOwnerFailover() {
  formNetwork();
  bindSensorA(true);
  runFor(8000);
  const uint32_t t0 = nowMs();
  probeCommand("FAULT POWER_CYCLE 1 40");
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] FAULT_EVENT node=1 kind=POWER_CYCLE state=STARTED duration_ms=40000", t0); }, 1000));

  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] WIRELESS_FAILOVER uuid=0xE6616408432B6F39 from=1 to=2 reason=OWNER_LOST", t0); },
                     30000));
  const long decided = logTime(probe(), "[GW] WIRELESS_FAILOVER", t0);
  CHECK(runUntilTrue([t0] { return logHas(probe(), "TEL NODE=2 CHILD=1 ", t0); }, 45000));
  const long recovered = logTime(probe(), "TEL NODE=2 CHILD=1 ", t0);
  note("reassociacao: decisao em %ld ms, telemetria pelo novo Node em %ld ms", decided - static_cast<long>(t0),
       recovered - static_cast<long>(t0));
  CHECK(logHas(probe(), "WIRELESS_ASSOC node=2 child=1 uuid=0xE6616408432B6F39", t0));
  CHECK(logHas(probe(), "[GW] WIRELESS_LINK node=2 child=1 state=SECURE", t0));
  CHECK(g_sensors[0].parent == 2 && g_sensors[0].sessions == 2);

  // O Node 1 volta, sem vínculos, e não disputa o sensor.
  CHECK(runUntilTrue([] { return node(1).restarts == 1 && node(1).powered &&
                                 node(1).inst->nodeState() == ST_FOLLOWER; }, 40000));
  const uint32_t t1 = nowMs();
  runFor(25000);
  CHECK(!logHas(probe(), "TEL NODE=1 ", t1));
  CHECK(logCount(probe(), "TEL NODE=2 CHILD=1 ", t1) >= 20);
  CHECK(logCount(probe(), "[GW] WIRELESS_FAILOVER") == 1);
  CHECK(leaderCount() == 1 && currentLeader() == 3);
  return 0;
}

// O sensor sai do alcance do responsável (que continua ativo) e entra no de outro.
int scenarioSensorMoves() {
  formNetwork();
  Sensor& a = bindSensorA(true);
  (void)a;
  runFor(5000);
  const uint32_t t0 = nowMs();
  note("sensor deixa o alcance do node=1");
  g_sensors[0].rssi.erase(1);
  g_sensors[0].rssi[2] = -52;
  g_sensors[0].rssi[3] = -74;

  // Sem sessão e sem anúncios, o responsável declara o sensor STALE; o líder age em seguida.
  CHECK(runUntilTrue([t0] { return logHas(probe(), "WIRELESS_ASSOC node=1 child=1 uuid=0xE6616408432B6F39 profile=VIBRATION state=STALE", t0); },
                     30000));
  CHECK(logHas(probe(), "state=STALE rssi=-50 protocol=6 failover=AUTO session=NONE", t0));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] WIRELESS_FAILOVER uuid=0xE6616408432B6F39 from=1 to=2 reason=SENSOR_LOST rssi=-52", t0); },
                     5000));
  // O líder só pede a associação ao novo Node. O responsável anterior libera
  // o vínculo por conta própria quando ouve o novo publicá-lo.
  CHECK(runUntilTrue([t0] { return logHas(probe(), "WIRELESS_ASSOC node=2 child=1 uuid=0xE6616408432B6F39 profile=VIBRATION state=ASSOCIATING", t0); },
                     3000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "WIRELESS_ASSOC node=1 child=1 uuid=0xE6616408432B6F39 profile=VIBRATION state=UNBOUND", t0); },
                     3000));
  CHECK(logHas(node(1), "cedido ao NODE 2", t0));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "TEL NODE=2 CHILD=1 ", t0); }, 20000));
  note("reassociacao por alcance: telemetria pelo novo Node em %ld ms",
       logTime(probe(), "TEL NODE=2 CHILD=1 ", t0) - static_cast<long>(t0));
  runFor(15000);
  CHECK(logCount(probe(), "[GW] WIRELESS_FAILOVER") == 1);
  CHECK(g_sensors[0].parent == 2);
  return 0;
}

// Política MANUAL: o responsável sai do ar e ninguém assume o sensor.
int scenarioManualPolicy() {
  formNetwork();
  bindSensorA(false);
  CHECK(logHas(probe(), "failover=MANUAL"));
  runFor(5000);
  const uint32_t t0 = nowMs();
  probeCommand("FAULT POWER_CYCLE 1 60");
  runFor(60000);
  CHECK(!logHas(probe(), "[GW] WIRELESS_FAILOVER"));
  CHECK(!logHas(probe(), "TEL NODE=2 ", t0));
  CHECK(!logHas(probe(), "WIRELESS_ASSOC node=2 ", t0));
  return 0;
}

// O líder sai do ar: a Probe detecta, a rede elege outro e o antigo volta como seguidor.
int scenarioLeaderFailure() {
  formNetwork();
  runFor(5000);
  const uint32_t t0 = nowMs();
  probeCommand("FAULT POWER_CYCLE 3 20");
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] Falha detectada no lider NODE 3", t0); }, 8000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] HEARTBEAT lider=2", t0); }, 12000));
  note("lider substituido em %ld ms", logTime(probe(), "[GW] HEARTBEAT lider=2", t0) - static_cast<long>(t0));
  CHECK(currentLeader() == 2 && leaderCount() == 1);
  CHECK(runUntilTrue([] { return node(3).restarts == 1 && node(3).powered &&
                                 node(3).inst->nodeState() == ST_FOLLOWER; }, 30000));
  runFor(5000);
  CHECK(currentLeader() == 2 && leaderCount() == 1);
  CHECK(logHas(probe(), "[GW] JOIN observado de NODE 3", t0));
  return 0;
}

// O mesmo, com a Probe impedida de transmitir: os seguidores pedem a eleição.
int scenarioLeaderFailureNoProbe() {
  formNetwork();
  runFor(5000);
  const uint32_t t0 = nowMs();
  probeCommand("FAULT POWER_CYCLE 3 30");
  runFor(500);
  note("Probe 00 deixa de transmitir");
  probe().txDisconnected = true;
  CHECK(runUntilTrue([t0] { return logHas(node(2), "[VIGILANCIA] Lider NODE 3 sem heartbeat", t0); }, 9000));
  // O seguidor de maior identificador reage primeiro; o outro só recebe o pedido.
  CHECK(!logHas(node(1), "[VIGILANCIA]", t0));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] HEARTBEAT lider=2", t0); }, 12000));
  note("lider substituido sem a Probe em %ld ms",
       logTime(probe(), "[GW] HEARTBEAT lider=2", t0) - static_cast<long>(t0));
  CHECK(currentLeader() == 2 && leaderCount() == 1);
  runFor(10000);
  CHECK(logCount(node(2), "[VIGILANCIA]", t0) == 1);
  return 0;
}

// O líder congela e retoma acreditando que ainda é líder.
int scenarioSilenceLeader() {
  formNetwork();
  runFor(5000);
  const uint32_t t0 = nowMs();
  probeCommand("FAULT SILENCE 3 15");
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] FAULT_EVENT node=3 kind=SILENCE state=STARTED", t0); }, 1000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] HEARTBEAT lider=2", t0); }, 14000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] FAULT_EVENT node=3 kind=SILENCE state=ENDED", t0); }, 12000));
  CHECK(logHas(node(3), "[LACO] Parado por", t0));
  runFor(15000);
  // A disputa se resolve: um único líder, e ninguém foi marcado como falho pela parada.
  CHECK(leaderCount() == 1);
  note("apos o silencio: lider=%u", currentLeader());
  CHECK(!logHas(probe(), "estado=FALHA", t0 + 15000));
  CHECK(!logHas(node(3), "[OMISSAO]", t0));
  for (uint8_t id = 1; id <= 3; ++id) {
    const int st = node(id).inst->nodeState();
    CHECK(st == ST_LEADER || st == ST_FOLLOWER);
  }
  const uint32_t t1 = nowMs();
  runFor(5000);
  CHECK(logCount(probe(), "[GW] HEARTBEAT lider=", t1) >= 4);
  return 0;
}

// Ponto de acesso do Node desligado por alguns segundos.
int scenarioApOutage() {
  formNetwork();
  bindSensorA(true);
  runFor(5000);
  const uint32_t t0 = nowMs();
  probeCommand("FAULT AP_OUTAGE 1 8");
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] FAULT_EVENT node=1 kind=AP_OUTAGE state=STARTED duration_ms=8000", t0); }, 1000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] WIRELESS_LINK node=1 child=1 state=OFFERING", t0) ||
                                   logHas(probe(), "[GW] WIRELESS_LINK node=1 child=1 state=DOWN", t0); }, 3000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] FAULT_EVENT node=1 kind=AP_OUTAGE state=ENDED", t0); }, 10000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] WIRELESS_LINK node=1 child=1 state=SECURE", t0); }, 20000));
  note("sessao restabelecida em %ld ms",
       logTime(probe(), "[GW] WIRELESS_LINK node=1 child=1 state=SECURE", t0) - static_cast<long>(t0));
  const uint32_t t1 = nowMs();
  runFor(8000);
  CHECK(logCount(probe(), "TEL NODE=1 CHILD=1 ", t1) >= 6);
  // Interrupção curta: o sensor continua com o mesmo responsável.
  CHECK(!logHas(probe(), "[GW] WIRELESS_FAILOVER"));
  CHECK(g_sensors[0].sessions == 2);

  // Um segundo ensaio durante o primeiro é recusado; o cancelamento encerra antes do prazo.
  const uint32_t t2 = nowMs();
  probeCommand("FAULT AP_OUTAGE 1 60");
  runFor(1000);
  probeCommand("FAULT DROP_SESSION 1");
  CHECK(runUntilTrue([t2] { return logHas(probe(), "[GW] FAULT_EVENT node=1 kind=DROP_SESSION state=REJECTED", t2); }, 1000));
  probeCommand("FAULT CANCEL 1");
  CHECK(runUntilTrue([t2] { return logHas(probe(), "[GW] FAULT_EVENT node=1 kind=AP_OUTAGE state=ENDED", t2); }, 1000));
  return 0;
}

// O Node esquece as sessões; o sensor percebe pelo silêncio e se reautentica.
int scenarioDropSession() {
  formNetwork();
  bindSensorA(true);
  runFor(5000);
  const uint32_t t0 = nowMs();
  probeCommand("FAULT DROP_SESSION 1");
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] FAULT_EVENT node=1 kind=DROP_SESSION state=ENDED", t0); }, 1000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] WIRELESS_LINK node=1 child=1 state=OFFERING", t0); }, 3000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] WIRELESS_LINK node=1 child=1 state=SECURE", t0); }, 20000));
  note("sessao restabelecida em %ld ms",
       logTime(probe(), "[GW] WIRELESS_LINK node=1 child=1 state=SECURE", t0) - static_cast<long>(t0));
  CHECK(g_sensors[0].sessions == 2);
  CHECK(!logHas(probe(), "[GW] WIRELESS_FAILOVER"));
  return 0;
}

// Carga artificial de alta e de baixa prioridade gerada pela Probe.
int scenarioBusLoad() {
  formNetwork();
  bindSensorA(true);
  runFor(8000);
  const uint32_t t0 = nowMs();
  probeCommand("FAULT LOAD 40 10 HIGH");
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] FAULT_EVENT node=0 kind=BUS_LOAD state=STARTED duration_ms=10000", t0); }, 500));
  runFor(11000);
  CHECK(logHas(probe(), "[GW] FAULT_EVENT node=0 kind=BUS_LOAD state=ENDED", t0));
  const std::string* ended = logLast(probe(), "kind=BUS_LOAD state=ENDED");
  if (ended) {
    CHECK(field(*ended, "load_pct") == 40 && field(*ended, "achieved_pct") > 34.0 && field(*ended, "achieved_pct") < 42.0);
    CHECK(field(*ended, "frames") > 10000);
  }
  // A ocupação medida inclui o que a própria Probe transmitiu.
  double maxLoad = 0.0;
  for (const auto& entry : probe().log) {
    if (entry.first >= t0 && entry.second.find("[GW] BUS_STATS ") != std::string::npos) {
      maxLoad = std::max(maxLoad, field(entry.second, "load"));
    }
  }
  note("ocupacao maxima medida: %.2f %%", maxLoad);
  CHECK(maxLoad > 35.0 && maxLoad < 48.0);
  CHECK(logHas(probe(), "[GW] ID_STATS id=0x010 ", t0));
  // O tráfego do projeto continua, e a telemetria de menor prioridade atravessa.
  CHECK(logCount(probe(), "[GW] HEARTBEAT lider=3", t0) >= 9);
  CHECK(logCount(probe(), "TEL NODE=1 CHILD=1 ", t0) >= 8);
  CHECK(leaderCount() == 1 && currentLeader() == 3);

  const uint32_t t1 = nowMs();
  probeCommand("FAULT LOAD 30 5 LOW");
  runFor(2000);
  probeCommand("FAULT CANCEL");
  CHECK(runUntilTrue([t1] { return logHas(probe(), "kind=BUS_LOAD state=ENDED", t1); }, 500));
  const std::string* cancelled = logLast(probe(), "kind=BUS_LOAD state=ENDED");
  if (cancelled) CHECK(cancelled->find("priority=LOW") != std::string::npos && field(*cancelled, "duration_ms") < 2600);
  CHECK(logHas(probe(), "[GW] ID_STATS id=0x7EF ", t1) || runUntilTrue([t1] { return logHas(probe(), "[GW] ID_STATS id=0x7EF ", t1); }, 6000));

  probeCommand("FAULT LOAD 95 5");
  probeCommand("FAULT POWER_CYCLE 0 5");
  probeCommand("FAULT NADA 1 5");
  runFor(300);
  CHECK(logCount(probe(), "[GW] FAULT_ERROR", t1) == 3);
  return 0;
}

// Dois sensores, dois Nodes: o responsável por ambos sai do ar.
int scenarioTwoSensors() {
  formNetwork();
  bindSensorA(true);
  Sensor& b = addSensor(SENSOR_B, 4243, 3);
  b.rssi[1] = -60;
  b.rssi[3] = -58;
  CHECK(runUntilTrue([] { return logHas(probe(), "WIRELESS_CANDIDATE reporter=1 uuid=0xE66164084327A12C"); }, 8000));
  probeCommand("WIRELESS BIND 1 0xE66164084327A12C AUTO");
  CHECK(runUntilTrue([] { return logHas(probe(), "TEL NODE=1 CHILD=2 "); }, 15000));
  runFor(5000);
  const uint32_t t0 = nowMs();
  probeCommand("FAULT POWER_CYCLE 1 60");
  // Cada sensor vai para o Node que o alcança: A para o 2, B para o 3 (o líder, que executa em si mesmo).
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] WIRELESS_FAILOVER uuid=0xE6616408432B6F39 from=1 to=2", t0) &&
                                   logHas(probe(), "[GW] WIRELESS_FAILOVER uuid=0xE66164084327A12C from=1 to=3", t0); },
                     30000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "TEL NODE=2 CHILD=1 ", t0) &&
                                   logHas(probe(), "TEL NODE=3 CHILD=1 ", t0); }, 50000));
  CHECK(logCount(probe(), "[GW] WIRELESS_FAILOVER") == 2);
  return 0;
}

// O Node escolhido para receber o sensor deixa de ouvi-lo logo antes da
// decisão. O vínculo não pode ficar preso a ele: o líder tenta outro Node.
int scenarioFailoverRetry() {
  formNetwork();
  bindSensorA(true);
  runFor(5000);
  const uint32_t t0 = nowMs();
  note("sensor deixa o alcance do node=1");
  g_sensors[0].rssi.erase(1);
  g_sensors[0].rssi[2] = -52;
  g_sensors[0].rssi[3] = -74;
  // O Node 1 leva 25 s para declarar o vínculo STALE. O Node 2 perde o
  // sensor 3 s antes: a última observação dele ainda vale para o líder.
  runFor(22000);
  note("sensor deixa o alcance do node=2");
  g_sensors[0].rssi.erase(2);

  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] WIRELESS_FAILOVER uuid=0xE6616408432B6F39 from=1 to=2 reason=SENSOR_LOST", t0); },
                     25000));
  // O Node 2 aceita com a observação que tinha, não ouve mais o sensor e
  // declara o vínculo STALE em vez de permanecer em ASSOCIATING.
  CHECK(runUntilTrue([t0] { return logHas(probe(), "WIRELESS_ASSOC node=2 child=1 uuid=0xE6616408432B6F39 profile=VIBRATION state=STALE", t0); },
                     25000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] WIRELESS_FAILOVER uuid=0xE6616408432B6F39 from=2 to=3 reason=SENSOR_LOST rssi=-74", t0); },
                     40000));
  const long first = logTime(probe(), "from=1 to=2 reason=SENSOR_LOST", t0);
  const long second = logTime(probe(), "from=2 to=3 reason=SENSOR_LOST", t0);
  CHECK(second - first >= static_cast<long>(IOC_FO_COOLDOWN_MS));   // intervalo mínimo entre decisões
  CHECK(runUntilTrue([t0] { return logHas(probe(), "TEL NODE=3 CHILD=1 ", t0); }, 30000));
  note("segunda tentativa: telemetria pelo node=3 em %ld ms",
       logTime(probe(), "TEL NODE=3 CHILD=1 ", t0) - static_cast<long>(t0));
  runFor(12000);
  CHECK(g_sensors[0].parent == 3 && logCount(probe(), "[GW] WIRELESS_FAILOVER") == 2);
  // No final só o Node 3 mantém o vínculo.
  const uint32_t t1 = nowMs() - 11000;
  CHECK(!logHas(probe(), "WIRELESS_ASSOC node=1 child=1", t1) && !logHas(probe(), "WIRELESS_ASSOC node=2 child=1", t1));
  CHECK(logHas(probe(), "WIRELESS_ASSOC node=3 child=1 uuid=0xE6616408432B6F39 profile=VIBRATION state=ONLINE", t1));
  return 0;
}

// O Node escolhido sai do ar logo antes da decisão: o pedido fica sem
// resposta. O responsável anterior mantém o vínculo e o líder tenta outro.
int scenarioFailoverDeadTarget() {
  formNetwork();
  bindSensorA(true);
  runFor(5000);
  const uint32_t t0 = nowMs();
  note("sensor deixa o alcance do node=1");
  g_sensors[0].rssi.erase(1);
  g_sensors[0].rssi[2] = -52;
  g_sensors[0].rssi[3] = -74;
  // O Node 2 sai 3 s antes da decisão: menos que o prazo de ausência (5 s),
  // de modo que o líder ainda o considera presente e com observação válida.
  runFor(22000);
  probeCommand("FAULT POWER_CYCLE 2 60");

  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] WIRELESS_FAILOVER uuid=0xE6616408432B6F39 from=1 to=2 reason=SENSOR_LOST", t0); },
                     15000));
  // Sem resposta do Node 2, o vínculo continua com o Node 1 (ninguém o desfez).
  runFor(12000);
  CHECK(!logHas(probe(), "WIRELESS_ASSOC node=1 child=1 uuid=0xE6616408432B6F39 profile=VIBRATION state=UNBOUND", t0));
  CHECK(logHas(probe(), "WIRELESS_ASSOC node=1 child=1 uuid=0xE6616408432B6F39", nowMs() - 11000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] WIRELESS_FAILOVER uuid=0xE6616408432B6F39 from=1 to=3 reason=SENSOR_LOST", t0); },
                     40000));
  CHECK(runUntilTrue([t0] { return logHas(probe(), "TEL NODE=3 CHILD=1 ", t0); }, 30000));
  note("destino fora do ar: telemetria pelo node=3 em %ld ms",
       logTime(probe(), "TEL NODE=3 CHILD=1 ", t0) - static_cast<long>(t0));
  CHECK(g_sensors[0].parent == 3 && logCount(probe(), "[GW] WIRELESS_FAILOVER") == 2);
  return 0;
}

// O líder congela por mais tempo que o prazo de silêncio de um vínculo e
// retoma: ele não pode reassociar um sensor que nunca deixou de funcionar.
int scenarioSilenceLeaderWithSensor() {
  formNetwork();
  bindSensorA(true);
  g_sensors[0].rssi[3] = -40;      // o líder ouve o sensor melhor que o responsável
  runFor(8000);
  const uint32_t t0 = nowMs();
  probeCommand("FAULT SILENCE 3 43");
  CHECK(runUntilTrue([t0] { return logHas(probe(), "[GW] FAULT_EVENT node=3 kind=SILENCE state=ENDED", t0); }, 46000));
  CHECK(logHas(node(3), "[LACO] Parado por", t0));
  runFor(40000);
  CHECK(!logHas(probe(), "[GW] WIRELESS_FAILOVER"));
  CHECK(!logHas(node(3), "[WIRELESS FAILOVER]"));
  CHECK(g_sensors[0].parent == 1 && g_sensors[0].sessions == 1);
  CHECK(logCount(probe(), "TEL NODE=1 CHILD=1 ", t0) >= 75);
  CHECK(!logHas(probe(), "WIRELESS_ASSOC node=3 ", t0) && !logHas(probe(), "WIRELESS_ASSOC node=2 ", t0));
  CHECK(leaderCount() == 1);
  return 0;
}

struct Scenario {
  const char* name;
  int (*run)();
  const char* description;
};

const Scenario SCENARIOS[] = {
  {"formation", scenarioFormation, "formacao da rede e estatisticas do barramento"},
  {"sensor", scenarioSensor, "sensor associado, tempo de resposta e politica de reassociacao"},
  {"owner_failover", scenarioOwnerFailover, "Node responsavel sai do ar; o lider reassocia o sensor"},
  {"sensor_moves", scenarioSensorMoves, "sensor muda de alcance; o lider reassocia"},
  {"manual_policy", scenarioManualPolicy, "politica MANUAL: sem reassociacao automatica"},
  {"leader_failure", scenarioLeaderFailure, "lider sai do ar e volta como seguidor"},
  {"leader_failure_no_probe", scenarioLeaderFailureNoProbe, "lider sai do ar com a Probe muda"},
  {"silence_leader", scenarioSilenceLeader, "lider congela e retoma"},
  {"ap_outage", scenarioApOutage, "ponto de acesso desligado e religado"},
  {"drop_session", scenarioDropSession, "sessoes descartadas pelo Node"},
  {"bus_load", scenarioBusLoad, "carga artificial no barramento"},
  {"two_sensors", scenarioTwoSensors, "dois sensores redistribuidos entre os Nodes"},
  {"failover_retry", scenarioFailoverRetry, "o Node escolhido perde o sensor; o lider tenta outro"},
  {"failover_dead_target", scenarioFailoverDeadTarget, "o Node escolhido sai do ar; o vinculo anterior e mantido"},
  {"silence_leader_with_sensor", scenarioSilenceLeaderWithSensor, "lider retoma de uma parada longa sem reassociar"},
};

}  // namespace

uint64_t sim_now_us() { return g_now_us; }

// delay() chamado pelo firmware: o tempo passa e o barramento continua.
void sim_delay_ms(uint32_t ms) {
  const uint64_t until = g_now_us + static_cast<uint64_t>(ms) * 1000u;
  while (g_now_us < until) {
    pumpBus(g_now_us + TICK_US);
    g_now_us += TICK_US;
  }
}

uint32_t sim_random32() {
  g_rng = g_rng * 1664525u + 1013904223u;
  return g_rng;
}

int main(int argc, char** argv) {
  const char* name = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--verbose") == 0) g_verbose = true;
    else if (strcmp(argv[i], "--list") == 0) {
      for (const Scenario& s : SCENARIOS) printf("%s\t%s\n", s.name, s.description);
      return 0;
    } else name = argv[i];
  }
  if (!name) {
    fprintf(stderr, "Uso: %s <cenario> [--verbose] | --list\n", argv[0]);
    return 2;
  }

  if (!ioc_link_hex_to_key(IOT_LINK_MASTER_KEY_HEX, g_master)) return 2;
  ioc_link_derive_network_key(g_master, g_network_key);

  for (const Scenario& s : SCENARIOS) {
    if (strcmp(s.name, name) != 0) continue;
    printf("# cenario=%s: %s\n", s.name, s.description);
    setupBench();
    s.run();
    for (Node& n : g_nodes) collectSerial(n);
    printf("# fim t=%u quadros=%u falhas=%d\n", nowMs(), g_bus_frames, g_failures);
    if (g_failures) {
      fprintf(stderr, "sim_network %s: %d verificacao(oes) falharam\n", s.name, g_failures);
      return 1;
    }
    fprintf(stderr, "sim_network %s: PASS\n", s.name);
    return 0;
  }
  fprintf(stderr, "Cenario desconhecido: %s (use --list)\n", name);
  return 2;
}
