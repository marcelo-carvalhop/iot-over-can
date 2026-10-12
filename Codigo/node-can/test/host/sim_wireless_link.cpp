/*
 * Simulação no computador do caminho de dados wireless:
 *
 *   sensor simulado <-UDP-> wireless_link.cpp (Node 1) <-CAN-> wireless_link.cpp (Probe 00)
 *
 * O código de wireless_link.cpp é o mesmo do firmware; rádio, UDP e CAN são
 * substituídos pelos arquivos em stubs/. O sensor simulado usa a biblioteca
 * ioc_link e as estruturas de edge_protocol_definitions.h, como o Pico W.
 *
 * A saída padrão contém exatamente as linhas que a Probe 00 enviaria à TUI;
 * Front/tests/test_wireless_data_plane_v017.py as entrega ao decodificador
 * real da TUI. Qualquer verificação interna que falhe encerra com código 1.
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "Arduino.h"
#include "ACAN2515.h"
#include "NimBLEDevice.h"
#include "WiFi.h"
#include "WiFiUdp.h"
#include "esp_wifi.h"

#include "can_ids.h"
#include "wireless_link.h"

#include "ioc_link.h"
#include "ioc_wdata.h"

extern "C" {
#include "../../../node-wifi/edge_protocol_definitions.h"
}

/* ------------------------------------------------------------ ambiente */

uint8_t NODE_ID = 1;
ACAN2515 can;
HostSerial Serial;
HostWiFi WiFi;
std::deque<HostDatagram> WiFiUDP::inbox;
std::deque<HostDatagram> WiFiUDP::outbox;

HostEsp ESP;

static uint32_t g_millis = 1000;
uint32_t millis() { return g_millis; }
uint32_t micros() { return g_millis * 1000u; }
void delay(uint32_t ms) { g_millis += ms; }
void HostEsp::restart() {}

int HostSerial::printf(const char* fmt, ...) {
  char buf[2048];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  captured += buf;
  return n;
}
void HostSerial::println(const char* text) { captured += text; captured += "\n"; }
void HostSerial::print(const char* text) { captured += text; }

static uint32_t g_rng = 0x1234567u;
void esp_fill_random(void* buf, size_t len) {
  uint8_t* out = static_cast<uint8_t*>(buf);
  for (size_t i = 0; i < len; ++i) {
    g_rng = g_rng * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(g_rng >> 24);
  }
}

int esp_wifi_ap_get_sta_list(wifi_sta_list_t* list) {
  list->num = 1;
  list->sta[0].rssi = -47;
  return ESP_OK;
}

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      g_failures++;                                                          \
    }                                                                        \
  } while (0)

/* ------------------------------------------------------ sensor simulado */

static const uint64_t SENSOR_UUID = 0xE6616408432B6F39ull;
static const uint64_t MIC_UUID = 0xE6616408432B7A11ull;   // sensor acústico (INMP441)
static const IPAddress SENSOR_IP(192, 168, 4, 2);
static const IPAddress MIC_IP(192, 168, 4, 3);
static const uint16_t SENSOR_PORT = 4242;

struct SimSensor {
  uint64_t uuid = SENSOR_UUID;
  uint8_t profile = PROFILE_ACCELEROMETER;
  IPAddress ip = SENSOR_IP;   // cada sensor recebe o próprio endereço do DHCP do AP
  uint8_t devKey[32];
  uint8_t sessionKey[32];
  uint8_t nonce[8];
  bool bound = false;
  uint32_t tx = 0;
  uint32_t rxLast = 0;
  uint16_t seq = 0;
  uint8_t nodeId = 0;
  uint8_t childId = 0;
  bool released = false;
  int pings = 0;
  int authRejects = 0;
  int menuRequests = 0;
  bool answerMenuRequests = true;
  bool sendMenuAfterConfirm = true;
  Payload_Configuration lastConfig{};
  bool gotConfig = false;
  Payload_AcousticControl lastControl{};
  int controls = 0;
  std::vector<uint8_t> lastSent;

  void send(const uint8_t* key, uint8_t type, uint32_t counter, const void* payload, size_t len) {
    std::vector<uint8_t> wire(len + IOC_ENV_OVERHEAD);
    size_t n = ioc_env_seal(key, type, counter, static_cast<const uint8_t*>(payload), len,
                            wire.data(), wire.size());
    CHECK(n == wire.size());
    HostDatagram d;
    d.data = wire;
    d.ip = ip;
    d.port = SENSOR_PORT;
    WiFiUDP::inbox.push_back(d);
    lastSent = wire;
  }

  void sendData(const void* inner, size_t len) { send(sessionKey, IOC_MSG_DATA_UP, ++tx, inner, len); }

  void hello() {
    for (int i = 0; i < 8; ++i) nonce[i] = static_cast<uint8_t>(0x40 + i + seq);
    ioc_hello_t h;
    h.uuid = uuid;
    h.profile = profile;
    h.protocol = NET_PROTOCOL_VERSION;
    memcpy(h.sensor_nonce, nonce, 8);
    uint8_t raw[IOC_HELLO_LEN];
    ioc_hello_encode(&h, raw);
    send(devKey, IOC_MSG_HELLO, 0, raw, sizeof(raw));
  }

  void configMenu() {
    Payload_ConfigMenu menu{};
    menu.magic_header = NET_MAGIC_HEADER;
    menu.cmd_type = CMD_CONFIG_MENU;
    if (profile == PROFILE_ACOUSTIC_MEMS) {
      menu.current_buffer_size = 1024;
      menu.current_fsm_mode = ACOUSTIC_MODE_LEVEL;
      menu.current_window_type = 1;
      menu.current_sample_rate_hz = 16000.0f;
      menu.current_stalta_thresh = 3.0f;
    } else {
      menu.current_buffer_size = 512;
      menu.current_fsm_mode = FSM_MODE_ROTATING_MACH;
      menu.current_window_type = 1;
      menu.current_sample_rate_hz = 1000.0f;
      menu.current_stalta_thresh = 4.0f;
    }
    menu.current_calib_gain = 1.0f;
    sendData(&menu, sizeof(menu));
  }

  // Relatório de um segundo do sensor acústico, com espectro opcional.
  void acoustic(uint8_t mode, uint16_t bins) {
    std::vector<uint8_t> inner(sizeof(Payload_AcousticStream) + bins * sizeof(float));
    Payload_AcousticStream a{};
    a.magic_header = NET_MAGIC_HEADER;
    a.cmd_type = CMD_ACOUSTIC_STREAM;
    a.seq_num = seq++;
    a.mode = mode;
    a.flags = ACOUSTIC_FLAG_SPECTRUM_VALID;
    a.window_type = 1;
    a.fft_size = 1024;
    a.sample_rate_req_hz = 16000.0f;
    a.sample_rate_eff_hz = 16000.0f;
    a.interval_ms = 1000;
    a.coverage_pct = 100;
    a.cpu_pct = 37;
    a.laeq_cb = 6240;
    a.lceq_cb = 6815;
    a.lzeq_cb = 6990;
    a.lzpeak_cb = 8312;
    a.laf_cb = 6105;
    a.lafmax_cb = 6650;
    a.lafmin_cb = 5820;
    a.crest_x100 = 412;
    a.kurtosis_x100 = -35;
    a.peak_hz = 1000.0f;
    a.centroid_hz = 1480;
    a.flatness_x10000 = 1234;
    a.leq_total_cb = a.l10_cb = a.l50_cb = a.l90_cb = ACOUSTIC_CB_NA;
    a.tonal_cb = a.anomaly_x100 = ACOUSTIC_CB_NA;
    a.event_peak_cb = a.event_sel_cb = a.sta_lta_cb = a.background_cb = ACOUSTIC_CB_NA;
    a.event_age_s = ACOUSTIC_AGE_NONE;
    if (mode == ACOUSTIC_MODE_LEVEL) {
      a.leq_total_cb = 6310;
      a.l10_cb = 6600;
      a.l50_cb = 6200;
      a.l90_cb = 5900;
      a.dose_nr15_x10 = 3;
      a.dose_nho01_x10 = 5;
      a.elapsed_s = 3600;
    } else if (mode == ACOUSTIC_MODE_MACHINE) {
      a.flags |= ACOUSTIC_FLAG_BASELINE_READY | ACOUSTIC_FLAG_ANOMALY;
      a.tonal_cb = 1850;
      a.f0_hz = 49.75f;
      a.harmonics = 6;
      a.anomaly_x100 = 512;
      a.baseline_pct = 100;
    } else if (mode == ACOUSTIC_MODE_EVENTS) {
      a.flags |= ACOUSTIC_FLAG_EVENT_ACTIVE;
      a.event_count = 7;
      a.events_interval = 1;
      a.event_class = ACOUSTIC_EVENT_CRACK;
      a.event_peak_cb = 9120;
      a.event_sel_cb = 7480;
      a.event_duration_ms = 40;
      a.event_centroid_hz = 5200;
      a.event_age_s = 0;
      a.sta_lta_cb = 1210;
      a.background_cb = 4410;
    }
    for (int b = 0; b < ACOUSTIC_OCTAVE_BANDS; ++b) a.octave_cb[b] = static_cast<int16_t>(4000 + 250 * b);
    a.octave_cb[9] = ACOUSTIC_CB_NA; // 16 kHz acima de fs/2
    a.battery_pct = 88;
    a.battery_mv = 3910;
    a.dtc_count = 1;
    a.dtc_active_code = DTC_SENS_ACOUSTIC_OVERLOAD;
    a.spectrum_bins = bins;
    memcpy(inner.data(), &a, sizeof(a));
    for (uint16_t i = 0; i < bins; ++i) {
      const float v = 0.002f + (i == 32 ? 0.5f : 0.0f);
      memcpy(inner.data() + sizeof(a) + i * sizeof(float), &v, sizeof(float));
    }
    sendData(inner.data(), inner.size());
  }

  void telemetry(bool withFft) {
    std::vector<uint8_t> inner(sizeof(Payload_TelemetryStream) + (withFft ? 256 * sizeof(float) : 0));
    Payload_TelemetryStream t{};
    t.magic_header = NET_MAGIC_HEADER;
    t.cmd_type = CMD_TELEMETRY_STREAM;
    t.seq_num = seq++;
    t.fsm_mode = FSM_MODE_ROTATING_MACH;
    t.acquisition_mode = ACQ_MODE_POLLING;
    t.axis_mask = AXIS_MASK_VECTOR;
    t.fft_valid = withFft ? 1 : 0;
    t.window_size = 512;
    t.sample_rate_req_hz = 1000.0f;
    t.sample_rate_eff_hz = 1000.0f;
    t.rms_ac = 0.1242f;
    t.kurtosis = -0.208f;
    t.crest_factor = 2.796f;
    t.peak_freq_hz = 31.25f;
    t.peak_amplitude = 0.02045f;
    t.entropy = 0.45f;
    t.ppv_max_mm_s = 0.4f;
    t.battery_pct = 93;
    t.battery_mv = 3980;
    t.dtc_count = 0;
    t.dtc_active_code = 0;
    t.fft_bins_count = withFft ? 256 : 0;
    memcpy(inner.data(), &t, sizeof(t));
    if (withFft) {
      float mags[256];
      for (int i = 0; i < 256; ++i) {
        mags[i] = 1000.0f * std::exp(-std::pow((i - 16) / 3.0f, 2.0f)) + 2.0f;
      }
      memcpy(inner.data() + sizeof(t), mags, sizeof(mags));
    }
    sendData(inner.data(), inner.size());
  }

  void urgentDtc() {
    uint8_t buf[3 + sizeof(DTC_Record)];
    uint16_t magic = NET_MAGIC_HEADER;
    DTC_Record rec{};
    rec.dtc_code = DTC_SENS_CLIPPING;
    rec.symptom = FTB_OUT_OF_RANGE_HIGH;
    rec.severity = SEV_WARNING;
    rec.freeze.timestamp_ms = 123456;
    memcpy(buf, &magic, 2);
    buf[2] = CMD_URGENT_DTC_ALARM;
    memcpy(buf + 3, &rec, sizeof(rec));
    sendData(buf, sizeof(buf));
  }

  // Processa o que o Node enviou, como faria o firmware do Pico W.
  void receive() {
    // Datagramas para outro sensor do mesmo AP ficam na fila para ele.
    decltype(WiFiUDP::outbox) mine;
    decltype(WiFiUDP::outbox) others;
    for (const HostDatagram& d : WiFiUDP::outbox) {
      if (d.ip == ip) mine.push_back(d);
      else others.push_back(d);
    }
    WiFiUDP::outbox = others;
    while (!mine.empty()) {
      HostDatagram d = mine.front();
      mine.pop_front();
      CHECK(d.port == SENSOR_PORT && d.ip == ip);
      ioc_env_view view;
      if (!ioc_env_parse(d.data.data(), d.data.size(), &view)) { CHECK(false); continue; }

      if (view.type == IOC_MSG_CHALLENGE) {
        if (!ioc_env_verify(devKey, d.data.data(), d.data.size())) { authRejects++; continue; }
        ioc_challenge_t ch;
        CHECK(ioc_challenge_decode(view.payload, view.payload_len, &ch));
        if (memcmp(ch.sensor_nonce, nonce, 8) != 0) continue; // resposta a um HELLO antigo
        ioc_link_derive_session_key(devKey, uuid, ch.node_id, nonce, ch.node_nonce, sessionKey);
        nodeId = ch.node_id;
        childId = ch.child_id;
        bound = true;
        tx = 0;
        rxLast = 0;
        ioc_confirm_t cf;
        memcpy(cf.node_nonce, ch.node_nonce, 8);
        memcpy(cf.sensor_nonce, nonce, 8);
        uint8_t raw[IOC_CONFIRM_LEN];
        ioc_confirm_encode(&cf, raw);
        send(sessionKey, IOC_MSG_CONFIRM, ++tx, raw, sizeof(raw));
        if (sendMenuAfterConfirm) configMenu();
        continue;
      }

      if (view.type != IOC_MSG_DATA_DOWN || !bound) continue;
      if (!ioc_env_verify(sessionKey, d.data.data(), d.data.size())) { authRejects++; continue; }
      CHECK(ioc_counter_accept(&rxLast, view.counter));
      const uint8_t* inner = view.payload;
      CHECK(view.payload_len >= 3);
      switch (inner[2]) {
        case CMD_PING: {
          pings++;
          Payload_Heartbeat ping{};
          if (view.payload_len >= sizeof(ping)) memcpy(&ping, inner, sizeof(ping));
          Payload_Heartbeat pong{};
          pong.magic_header = NET_MAGIC_HEADER;
          pong.cmd_type = CMD_PONG;
          pong.request_counter = ping.request_counter; // o sensor devolve o contador do PING
          sendData(&pong, sizeof(pong));
          break;
        }
        case CMD_SET_CONFIG: {
          CHECK(view.payload_len == sizeof(Payload_Configuration));
          memcpy(&lastConfig, inner, sizeof(lastConfig));
          gotConfig = true;
          const bool valid = profile == PROFILE_ACOUSTIC_MEMS
                                 ? lastConfig.sample_rate_hz >= 8000.0f && lastConfig.sample_rate_hz <= 48000.0f &&
                                       lastConfig.target_mode <= ACOUSTIC_MODE_EVENTS
                                 : lastConfig.sample_rate_hz >= 4.0f && lastConfig.sample_rate_hz <= 1000.0f;
          for (uint8_t status = 0; status <= 1; ++status) {
            Payload_ConfigAck ack{};
            ack.magic_header = NET_MAGIC_HEADER;
            ack.cmd_type = CMD_ACK_CONFIG;
            ack.request_counter = lastConfig.request_counter;
            ack.status = valid ? status : 2;
            ack.target_mode = lastConfig.target_mode;
            ack.requested_window_size = lastConfig.window_size;
            ack.effective_window_size = lastConfig.window_size;
            ack.requested_sample_rate_hz = lastConfig.sample_rate_hz;
            ack.effective_sample_rate_hz = lastConfig.sample_rate_hz;
            sendData(&ack, sizeof(ack));
            if (!valid) break;
          }
          break;
        }
        case CMD_CLEAR_DTC: {
          Payload_DTCSnapshot snap{};
          snap.magic_header = NET_MAGIC_HEADER;
          snap.cmd_type = CMD_DTC_SNAPSHOT;
          sendData(&snap, sizeof(snap));
          break;
        }
        case CMD_GET_CONFIG_MENU:
          menuRequests++;
          if (answerMenuRequests) configMenu();
          break;
        case CMD_ACOUSTIC_CONTROL: {
          CHECK(view.payload_len == sizeof(Payload_AcousticControl));
          memcpy(&lastControl, inner, sizeof(lastControl));
          controls++;
          Payload_AcousticControlAck ack{};
          ack.magic_header = NET_MAGIC_HEADER;
          ack.cmd_type = CMD_ACOUSTIC_CONTROL_ACK;
          ack.request_counter = lastControl.request_counter;
          ack.action = lastControl.action;
          // Só o perfil acústico conhece o comando; a referência exige 5 s ou mais.
          const bool ok = profile == PROFILE_ACOUSTIC_MEMS &&
                          (lastControl.action != ACOUSTIC_CTRL_BASELINE_LEARN || lastControl.argument >= 5);
          ack.status = ok ? 1 : 2;
          sendData(&ack, sizeof(ack));
          break;
        }
        case CMD_RELEASE:
          released = true;
          bound = false;
          break;
        default:
          break;
      }
    }
  }
};

/* --------------------------------------------------------- orquestração */

static std::string g_probe_output;

static std::vector<CANMessage> takeCanFrames() {
  std::vector<CANMessage> frames;
  frames.swap(can.sent);
  can.pendingInDriver = 0;
  return frames;
}

static void asNode() { NODE_ID = 1; }
static void asProbe() { NODE_ID = 0; }

// Entrega ao outro papel os quadros que o papel atual transmitiu.
static size_t deliverFrames(bool toProbe) {
  std::vector<CANMessage> frames = takeCanFrames();
  if (toProbe) asProbe(); else asNode();
  Serial.captured.clear();
  for (const CANMessage& f : frames) CHECK(wirelessLinkHandleCanMessage(f));
  if (toProbe) {
    g_probe_output += Serial.captured;
    fputs(Serial.captured.c_str(), stdout);
  }
  std::string out = Serial.captured;
  Serial.captured.clear();
  return frames.size();
}

// Um ciclo completo: Node recebe UDP, transmite CAN em rajadas limitadas pelo
// buffer do driver, a Probe remonta e o sensor responde.
static size_t cycle(SimSensor& sensor, int rounds = 12) {
  size_t frames = 0;
  for (int i = 0; i < rounds; ++i) {
    asNode();
    wirelessLinkPoll();
    sensor.receive();
    frames += deliverFrames(true);
    g_millis += 5;
  }
  return frames;
}

static void probeCommand(const char* line) {
  asProbe();
  Serial.captured.clear();
  CHECK(wirelessLinkHandleSerialCommand(line));
  g_probe_output += Serial.captured;
  fputs(Serial.captured.c_str(), stdout);
  deliverFrames(false); // comandos da Probe chegam ao Node
}

static bool probeSaw(const char* text) { return g_probe_output.find(text) != std::string::npos; }

int main() {
  uint8_t master[32], net[32];
  CHECK(ioc_link_hex_to_key(IOT_LINK_MASTER_KEY_HEX, master));
  ioc_link_derive_network_key(master, net);

  SimSensor sensor;
  ioc_link_derive_device_key(master, SENSOR_UUID, sensor.devKey);

  asProbe();
  wirelessLinkInit();
  CHECK(!wirelessLinkHandleSerialCommand("WIRELESS BIND 1 0xE6616408432B6F39"));
  CHECK(!wirelessLinkHandleSerialCommand("22 20 FF 00"));
  asNode();
  wirelessLinkInit();
  CHECK(wirelessLinkHasKey());
  CHECK(!WiFi.apUp);

  /* 1. Associação: o Node abre o AP e anuncia a oferta autenticada. */
  wirelessLinkOnBound(SENSOR_UUID, 1);
  wirelessLinkPoll();
  char psk[IOC_LINK_PSK_LEN];
  ioc_link_wifi_psk(net, 1, psk);
  CHECK(WiFi.apUp && WiFi.ssid == "IOC-01" && WiFi.psk == psk && WiFi.channel == 6);
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  CHECK(adv->advertising);
  ioc_offer_t offer;
  CHECK(ioc_offer_verify(sensor.devKey, reinterpret_cast<const uint8_t*>(adv->manufacturer.data()),
                         adv->manufacturer.size(), &offer));
  CHECK(offer.uuid == SENSOR_UUID && offer.node_id == 1 && offer.channel == 6);
  deliverFrames(true);
  CHECK(probeSaw("[GW] WIRELESS_LINK node=1 child=1 state=OFFERING"));

  /* 2. Sem sessão, comandos de dados são recusados de forma explícita. */
  probeCommand("CMD TARGET=01.01 ACTION=FFT TX=A00001 MODE=VIEW_ONLY BINS=64");
  cycle(sensor, 2);
  CHECK(probeSaw("ACK COMMAND=FFT STATE=REJECTED TX=A00001 NODE=1 CHILD=1 DETAIL=0 REASON=NO_DATA_LINK"));

  /* 3. Handshake. */
  sensor.hello();
  cycle(sensor);
  CHECK(sensor.bound && sensor.nodeId == 1 && sensor.childId == 1);
  CHECK(probeSaw("[GW] WIRELESS_LINK node=1 child=1 state=SECURE rssi=-47"));
  CHECK(probeSaw("[GW] WIRELESS_CONFIG node=1 child=1 status=CURRENT mode=ROTATING"));
  CHECK(!adv->advertising); // com sessão, a oferta deixa de ser anunciada
  CHECK(sensor.menuRequests == 1); // o Node não depende do menu enviado espontaneamente
  CHECK(!WiFi.persistentCredentials);

  /* 4. Telemetria periódica: uma amostra por período chega ao CAN. */
  sensor.telemetry(true);
  size_t frames = cycle(sensor);
  CHECK(frames == ioc_wd_segment_count(IOC_WD_TELEMETRY_LEN));
  CHECK(probeSaw("TEL NODE=1 CHILD=1 SEQ=0 "));
  CHECK(probeSaw("MODE=ROTATING ACQ=POLLING WIN=512 AXIS=VECTOR FFT_VALID=YES"));
  CHECK(probeSaw("RMS=0.12420 RMS_UNIT=m/s2 KURT=-0.20800 CREST=2.7960 PEAK_HZ=31.250"));
  sensor.telemetry(true); // dentro do mesmo período: não é encaminhada
  CHECK(cycle(sensor) == 0);
  CHECK(!probeSaw("TEL NODE=1 CHILD=1 SEQ=1 "));
  g_millis += 1000;
  sensor.telemetry(true);
  cycle(sensor);
  // A sequência no CAN é a das amostras encaminhadas: a amostra descartada
  // pelo Node (a segunda do sensor) não aparece como perda.
  CHECK(probeSaw("TEL NODE=1 CHILD=1 SEQ=1 "));
  CHECK(!probeSaw("TEL NODE=1 CHILD=1 SEQ=2 "));

  /* 5. Espectro sob demanda, reduzido para 64 faixas. */
  probeCommand("CMD TARGET=01.01 ACTION=FFT TX=A00002 MODE=VIEW_ONLY BINS=64");
  cycle(sensor, 2);
  CHECK(probeSaw("ACK COMMAND=FFT STATE=ACCEPTED TX=A00002"));
  sensor.telemetry(true);
  cycle(sensor, 40);
  CHECK(probeSaw("FRAG NODE=1 CHILD=1 TYPE=FFT TRANSFER=1 INDEX=0 COUNT=1 FORMAT=U16_SCALED SCALE=1002.000000 "
                 "RATE_HZ=1000.00 FFT_SIZE=128 WINDOW=HANN DATA="));
  CHECK(probeSaw("ACK COMMAND=FFT STATE=APPLIED TX=A00002 NODE=1 CHILD=1 DETAIL=64"));

  /* 5b. 256 faixas: quatro transferências, limitadas pelo buffer do driver CAN. */
  probeCommand("CMD TARGET=01.01 ACTION=FFT TX=A00003 MODE=VIEW_ONLY BINS=256");
  cycle(sensor, 2);
  sensor.telemetry(true);
  size_t burstMax = 0;
  for (int i = 0; i < 120; ++i) { // o Node nunca ocupa o buffer do driver além da marca
    size_t n = cycle(sensor, 1);
    if (n > burstMax) burstMax = n;
  }
  CHECK(burstMax > 0 && burstMax <= 4);
  CHECK(probeSaw("TYPE=FFT TRANSFER=2 INDEX=3 COUNT=4 FORMAT=U16_SCALED"));
  CHECK(probeSaw("ACK COMMAND=FFT STATE=APPLIED TX=A00003 NODE=1 CHILD=1 DETAIL=256"));

  /* 6. Configuração remota: SETs intermediários silenciosos, APPLY confirmado pelo sensor. */
  probeCommand("CMD TARGET=01.01 ACTION=CONFIG TX=A00004 MODE=STRUCTURAL RATE_HZ=500 WINDOW=HAMMING "
               "WINDOW_SIZE=256 STALTA=3.5 GAIN=1.25 VERIFY=YES");
  cycle(sensor);
  CHECK(sensor.gotConfig);
  CHECK(sensor.lastConfig.target_mode == FSM_MODE_STRUCTURAL && sensor.lastConfig.sample_rate_hz == 500.0f);
  CHECK(sensor.lastConfig.window_type == 2 && sensor.lastConfig.window_size == 256);
  CHECK(sensor.lastConfig.stalta_thresh == 3.5f && sensor.lastConfig.calib_gain == 1.25f);
  CHECK(probeSaw("ACK COMMAND=CONFIG STATE=ACCEPTED TX=A00004"));
  CHECK(probeSaw("ACK COMMAND=CONFIG STATE=APPLIED TX=A00004 NODE=1 CHILD=1"));
  CHECK(probeSaw("[GW] WIRELESS_CONFIG node=1 child=1 status=APPLIED mode=STRUCTURAL rate_req_hz=500.00 "
                 "rate_eff_hz=500.00 window=HAMMING window_size=256 stalta=3.500 gain=1.2500"));

  /* 6b. Configuração inválida: o sensor recusa e a TUI é informada. */
  probeCommand("CMD TARGET=01.01 ACTION=CONFIG TX=A00005 RATE_HZ=5000");
  cycle(sensor);
  CHECK(probeSaw("ACK COMMAND=CONFIG STATE=REJECTED TX=A00005 NODE=1 CHILD=1 DETAIL=2 REASON=SENSOR_REJECTED"));
  CHECK(probeSaw("status=REJECTED mode=STRUCTURAL rate_req_hz=500.00"));

  /* 7. Controle do encaminhamento e demais ações. */
  probeCommand("CMD TARGET=01.01 ACTION=TELEMETRY TX=A00006 STATE=OFF");
  cycle(sensor, 3);
  CHECK(probeSaw("ACK COMMAND=TELEMETRY STATE=APPLIED TX=A00006"));
  CHECK(probeSaw("stream=OFF period_ms=1000"));
  g_millis += 2000;
  sensor.telemetry(false);
  size_t before = g_probe_output.size();
  cycle(sensor);
  CHECK(g_probe_output.find("TEL NODE", before) == std::string::npos);
  probeCommand("CMD TARGET=01.01 ACTION=TELEMETRY_PERIOD TX=A00007 STATE=PERIOD PERIOD_MS=500");
  cycle(sensor, 3);
  CHECK(probeSaw("stream=ON period_ms=500"));
  probeCommand("CMD TARGET=01.01 ACTION=DTC_CLEAR TX=A00008 SCOPE=ALL");
  cycle(sensor);
  CHECK(probeSaw("ACK COMMAND=DTC_CLEAR STATE=APPLIED TX=A00008"));
  sensor.urgentDtc();
  cycle(sensor);
  CHECK(probeSaw("DTC NODE=1 CHILD=1 CODE=0x2002 SYMPTOM=0x16 SEVERITY=1 TS_MS=123456"));
  probeCommand("CMD TARGET=01.01 ACTION=ACQ TX=A00009 MODE=POLLING");
  CHECK(probeSaw("ACK COMMAND=ACQ STATE=REJECTED TX=A00009 REASON=UNSUPPORTED_VIA_NODE"));
  probeCommand("CMD TARGET=99.01 ACTION=STATUS TX=A0000A");
  CHECK(probeSaw("ACK COMMAND=STATUS STATE=REJECTED TX=A0000A REASON=INVALID_TARGET"));
  probeCommand("CMD TARGET=01.03 ACTION=STATUS TX=A0000B");
  cycle(sensor, 3);
  CHECK(probeSaw("ACK COMMAND=STATUS STATE=REJECTED TX=A0000B NODE=1 CHILD=3 DETAIL=0 REASON=UNKNOWN_TARGET_OR_OPCODE"));

  /* 8. Ataques. */
  // 8a. Repetição de um datagrama autêntico já aceito.
  g_millis += 600;
  sensor.telemetry(false);
  std::vector<uint8_t> captured = sensor.lastSent;
  cycle(sensor);
  before = g_probe_output.size();
  HostDatagram replay;
  replay.data = captured;
  replay.ip = SENSOR_IP;
  replay.port = SENSOR_PORT;
  g_millis += 600;
  WiFiUDP::inbox.push_back(replay);
  cycle(sensor);
  CHECK(g_probe_output.find("TEL NODE", before) == std::string::npos);
  // 8b. Telemetria com um bit alterado (tag inválida).
  sensor.telemetry(false);
  WiFiUDP::inbox.back().data[20] ^= 0x01;
  cycle(sensor);
  CHECK(g_probe_output.find("TEL NODE", before) == std::string::npos);
  // 8c. Sensor com chave de outro dispositivo tenta abrir sessão.
  SimSensor intruder;
  ioc_link_derive_device_key(master, SENSOR_UUID + 1, intruder.devKey);
  intruder.hello(); // anuncia o UUID legítimo, mas assina com a chave errada
  cycle(intruder);
  CHECK(!intruder.bound);
  // 8d. HELLO autêntico repetido não derruba a sessão em vigor.
  sensor.hello();
  sensor.bound = true; // o sensor real ignoraria; aqui só interessa o lado do Node
  std::vector<uint8_t> helloCopy = sensor.lastSent;
  WiFiUDP::outbox.clear();
  asNode();
  wirelessLinkPoll();
  WiFiUDP::outbox.clear(); // CHALLENGE enviado ao "atacante" é descartado
  deliverFrames(true);
  g_millis += 600;
  sensor.telemetry(false);
  before = g_probe_output.size();
  cycle(sensor);
  CHECK(g_probe_output.find("TEL NODE", before) != std::string::npos);
  probeCommand("CMD TARGET=01.01 ACTION=STATUS TX=A0000C");
  cycle(sensor, 3);
  CHECK(probeSaw("auth_fail=2 replay=1"));

  /* 9. Liveness: o Node envia PING e derruba a sessão quando o sensor some. */
  int pingsBefore = sensor.pings;
  g_millis += 3100;
  cycle(sensor, 3);
  CHECK(sensor.pings > pingsBefore);
  WiFiUDP::inbox.clear();
  before = g_probe_output.size();
  for (int i = 0; i < 12; ++i) { // 12 s sem resposta do sensor
    g_millis += 1000;
    asNode();
    wirelessLinkPoll();
    WiFiUDP::outbox.clear();
    deliverFrames(true);
  }
  CHECK(g_probe_output.find("state=OFFERING", before) != std::string::npos);
  CHECK(adv->advertising);

  /* 10. Nova sessão após a queda, com o CONFIRM e o menu espontâneo perdidos. */
  sensor.bound = false;
  sensor.sendMenuAfterConfirm = false;
  sensor.answerMenuRequests = false;
  sensor.hello();
  std::vector<uint8_t> sessionHello = sensor.lastSent;
  asNode();
  wirelessLinkPoll();
  sensor.receive();              // sensor aceita o CHALLENGE e envia o CONFIRM...
  CHECK(sensor.bound && WiFiUDP::inbox.size() == 1);
  WiFiUDP::inbox.clear();        // ...que se perde no ar
  deliverFrames(true);
  g_millis += 600;
  sensor.telemetry(false);       // dados autenticados com a nova chave concluem a troca
  before = g_probe_output.size();
  cycle(sensor);
  CHECK(g_probe_output.find("state=SECURE", before) != std::string::npos);
  CHECK(g_probe_output.find("TEL NODE", before) != std::string::npos);

  // Sem a configuração atual do sensor, o Node recusa a alteração com motivo
  // próprio e volta a pedir o menu, em vez de ficar travado.
  probeCommand("CMD TARGET=01.01 ACTION=CONFIG TX=A0000D MODE=IDLE");
  cycle(sensor);
  CHECK(probeSaw("ACK COMMAND=CONFIG STATE=REJECTED TX=A0000D NODE=1 CHILD=1 DETAIL=65534 REASON=CONFIG_UNKNOWN"));
  CHECK(!probeSaw("ACK COMMAND=CONFIG STATE=ACCEPTED TX=A0000D"));
  sensor.answerMenuRequests = true;
  sensor.gotConfig = false;
  probeCommand("CMD TARGET=01.01 ACTION=STATUS TX=A0000E");
  cycle(sensor);
  probeCommand("CMD TARGET=01.01 ACTION=CONFIG TX=A0000F MODE=IDLE");
  cycle(sensor);
  CHECK(sensor.gotConfig && sensor.lastConfig.target_mode == FSM_MODE_IDLE);
  CHECK(sensor.lastConfig.window_size == 512); // demais campos vêm da configuração atual, não de edição antiga
  CHECK(probeSaw("ACK COMMAND=CONFIG STATE=APPLIED TX=A0000F"));

  /* 10b. Repetições de HELLO durante a sessão. */
  // O HELLO que originou a sessão em vigor é ignorado e contado como repetição.
  HostDatagram replayedHello;
  replayedHello.data = sessionHello;
  replayedHello.ip = SENSOR_IP;
  replayedHello.port = SENSOR_PORT;
  WiFiUDP::inbox.push_back(replayedHello);
  asNode();
  wirelessLinkPoll();
  CHECK(WiFiUDP::outbox.empty());  // nenhum CHALLENGE em resposta
  // Um HELLO autêntico mais antigo abre um handshake pendente; repetido em
  // seguida, recebe o mesmo CHALLENGE em vez de reiniciar a troca.
  replayedHello.data = helloCopy;  // capturado no passo 8d
  WiFiUDP::inbox.push_back(replayedHello);
  wirelessLinkPoll();
  CHECK(WiFiUDP::outbox.size() == 1);
  std::vector<uint8_t> firstChallenge = WiFiUDP::outbox.front().data;
  WiFiUDP::outbox.clear();
  WiFiUDP::inbox.push_back(replayedHello);
  wirelessLinkPoll();
  CHECK(WiFiUDP::outbox.size() == 1 && WiFiUDP::outbox.front().data == firstChallenge);
  WiFiUDP::outbox.clear();
  // Outro HELLO logo depois não substitui o handshake recém-iniciado.
  SimSensor other = sensor;
  other.seq = 999;
  WiFiUDP::inbox.clear();
  other.hello();
  wirelessLinkPoll();
  CHECK(WiFiUDP::outbox.empty());
  deliverFrames(true);
  // A sessão legítima continua válida depois de tudo isso.
  g_millis += 600;
  sensor.telemetry(false);
  before = g_probe_output.size();
  cycle(sensor);
  CHECK(g_probe_output.find("TEL NODE", before) != std::string::npos);

  /* 11. Desassociação: o sensor é liberado e o AP só cai depois de o aviso sair. */
  asNode();
  wirelessLinkOnUnbound(SENSOR_UUID, 1);
  CHECK(WiFi.apUp);              // desligamento adiado
  sensor.receive();
  CHECK(sensor.released);
  g_millis += 500;
  wirelessLinkPoll();
  deliverFrames(true);
  CHECK(!WiFi.apUp && !adv->advertising);
  CHECK(g_probe_output.rfind("state=DOWN") != std::string::npos);

  /* 12. Novo vínculo antes do prazo cancela o desligamento do AP. */
  asNode();
  wirelessLinkOnBound(SENSOR_UUID, 2);
  wirelessLinkOnUnbound(SENSOR_UUID, 2);
  wirelessLinkOnBound(SENSOR_UUID, 2);
  g_millis += 500;
  asNode();
  wirelessLinkPoll();
  CHECK(WiFi.apUp);
  wirelessLinkOnUnbound(SENSOR_UUID, 2);
  g_millis += 500;
  wirelessLinkPoll();
  CHECK(!WiFi.apUp);
  takeCanFrames();

  /* 13. Sensor acústico (perfil 0x02) no filho 3: modos e ações próprios. */
  SimSensor mic;
  mic.uuid = MIC_UUID;
  mic.profile = PROFILE_ACOUSTIC_MEMS;
  mic.ip = MIC_IP;
  ioc_link_derive_device_key(master, MIC_UUID, mic.devKey);
  asNode();
  wirelessLinkOnBound(MIC_UUID, 3);
  wirelessLinkPoll();
  CHECK(WiFi.apUp);
  deliverFrames(true);
  mic.hello();
  cycle(mic);
  CHECK(mic.bound && mic.childId == 3);
  CHECK(probeSaw("[GW] WIRELESS_CONFIG node=1 child=3 status=CURRENT mode=LEVEL rate_req_hz=16000.00 "
                 "rate_eff_hz=16000.00 window=HANN window_size=1024 stalta=3.000 gain=1.0000 profile=ACOUSTIC"));

  // Relatório de 108 bytes: 19 quadros no CAN, com as chaves do console do sensor.
  g_millis += 1000;
  mic.acoustic(ACOUSTIC_MODE_LEVEL, 0);
  frames = cycle(mic);
  CHECK(frames == ioc_wd_segment_count(IOC_WD_ACOUSTIC_LEN));
  CHECK(ioc_wd_segment_count(IOC_WD_ACOUSTIC_LEN) == 19);
  CHECK(probeSaw("TEL NODE=1 CHILD=3 SEQ=0 GW_TS_MS="));
  CHECK(probeSaw(" PROFILE=ACOUSTIC MODE=LEVEL ACQ=I2S WIN=1024 RATE_EFF_HZ=16000 INTERVAL_MS=1000 "
                 "COVERAGE_PCT=100 CPU_PCT=37 SPECTRUM_VALID=YES OVERLOAD=NO MIC_FAULT=NO LAEQ_DB=62.40 "
                 "LCEQ_DB=68.15 LZEQ_DB=69.90 LZPEAK_DB=83.12 LAF_DB=61.05 LAFMAX_DB=66.50 LAFMIN_DB=58.20 "
                 "CREST=4.12 KURT=-0.35 PEAK_HZ=1000.0 CENTROID_HZ=1480 FLATNESS=0.1234 "
                 "OCT_DB=40.0,42.5,45.0,47.5,50.0,52.5,55.0,57.5,60.0,- LEQ_T_DB=63.10 L10_DB=66.00 "
                 "L50_DB=62.00 L90_DB=59.00 DOSE_NR15_PCT=0.3 DOSE_NHO01_PCT=0.5 ELAPSED_S=3600 DOSE_LIMIT=NO "
                 "BATT_PCT=88 BATT_MV=3910 DTC=0x2003 DTC_COUNT=1"));
  g_millis += 1000;
  mic.acoustic(ACOUSTIC_MODE_MACHINE, 0);
  cycle(mic);
  CHECK(probeSaw("MODE=MACHINE ACQ=I2S"));
  CHECK(probeSaw(" TONAL_DB=18.50 F0_HZ=49.75 HARMONICS=6 ANOMALY_Z=5.12 BASELINE=READY BASELINE_PCT=100 "
                 "ANOMALY=YES BATT_PCT=88"));
  CHECK(!probeSaw("MODE=MACHINE ACQ=I2S WIN=1024 RATE_EFF_HZ=16000 INTERVAL_MS=1000 COVERAGE_PCT=100 CPU_PCT=37 "
                  "SPECTRUM_VALID=YES OVERLOAD=NO MIC_FAULT=NO LAEQ_DB=62.40 LCEQ_DB=68.15 LZEQ_DB=69.90 "
                  "LZPEAK_DB=83.12 LAF_DB=61.05 LAFMAX_DB=66.50 LAFMIN_DB=58.20 CREST=4.12 KURT=-0.35 "
                  "PEAK_HZ=1000.0 CENTROID_HZ=1480 FLATNESS=0.1234 OCT_DB=40.0,42.5,45.0,47.5,50.0,52.5,55.0,"
                  "57.5,60.0,- LEQ_T_DB"));
  g_millis += 1000;
  mic.acoustic(ACOUSTIC_MODE_EVENTS, 0);
  cycle(mic);
  CHECK(probeSaw(" EVENTS=7 EVENTS_INTERVAL=1 EVENT_ACTIVE=YES STA_LTA_DB=12.10 BACKGROUND_DB=44.10 "
                 "EVENT_CLASS=CRACK EVENT_PEAK_DB=91.20 EVENT_SEL_DB=74.80 EVENT_DURATION_MS=40 "
                 "EVENT_CENTROID_HZ=5200 EVENT_AGE_S=0 BATT_PCT=88"));

  // Espectro sob demanda, em Pa, pelo mesmo caminho da vibração.
  probeCommand("CMD TARGET=01.03 ACTION=FFT TX=B00001 MODE=VIEW_ONLY BINS=64");
  cycle(mic, 2);
  g_millis += 1000;
  mic.acoustic(ACOUSTIC_MODE_LEVEL, 256);   // o sensor envia até 256 faixas (cabe em um datagrama)
  cycle(mic, 40);
  CHECK(probeSaw("FRAG NODE=1 CHILD=3 TYPE=FFT TRANSFER=1 INDEX=0 COUNT=1 FORMAT=U16_SCALED"));
  CHECK(probeSaw("RATE_HZ=16000.00 FFT_SIZE=128 WINDOW=HANN DATA="));
  CHECK(probeSaw("ACK COMMAND=FFT STATE=APPLIED TX=B00001 NODE=1 CHILD=3 DETAIL=64"));

  // Modos do microfone por nome; THRESHOLD e FFT_SIZE são sinônimos de STALTA e WINDOW_SIZE.
  probeCommand("CMD TARGET=01.03 ACTION=CONFIG TX=B00002 MODE=EVENTS RATE_HZ=48000 FFT_SIZE=2048 THRESHOLD=4");
  cycle(mic);
  CHECK(mic.gotConfig && mic.lastConfig.target_mode == ACOUSTIC_MODE_EVENTS);
  CHECK(mic.lastConfig.sample_rate_hz == 48000.0f && mic.lastConfig.window_size == 2048);
  CHECK(mic.lastConfig.stalta_thresh == 4.0f);
  CHECK(probeSaw("ACK COMMAND=CONFIG STATE=APPLIED TX=B00002 NODE=1 CHILD=3"));
  CHECK(probeSaw("[GW] WIRELESS_CONFIG node=1 child=3 status=APPLIED mode=EVENTS rate_req_hz=48000.00 "
                 "rate_eff_hz=48000.00 window=HANN window_size=2048 stalta=4.000 gain=1.0000 profile=ACOUSTIC"));

  // Um modo de vibração não chega ao microfone: o Node recusa a edição inteira.
  mic.gotConfig = false;
  probeCommand("CMD TARGET=01.03 ACTION=CONFIG TX=B00003 MODE=SEISMIC GAIN=2");
  cycle(mic);
  CHECK(!mic.gotConfig);
  CHECK(probeSaw("ACK COMMAND=CONFIG STATE=REJECTED TX=B00003 NODE=1 CHILD=3 DETAIL=65533 REASON=PROFILE_MISMATCH"));
  CHECK(!probeSaw("ACK COMMAND=CONFIG STATE=APPLIED TX=B00003"));
  // ...e o contrário, no sensor de vibração.
  sensor.bound = false;
  asNode();
  wirelessLinkOnBound(SENSOR_UUID, 1);
  sensor.hello();
  cycle(sensor);
  CHECK(sensor.bound && sensor.childId == 1);
  sensor.gotConfig = false;
  probeCommand("CMD TARGET=01.01 ACTION=CONFIG TX=B00004 MODE=MACHINE");
  cycle(sensor);
  CHECK(!sensor.gotConfig);
  CHECK(probeSaw("ACK COMMAND=CONFIG STATE=REJECTED TX=B00004 NODE=1 CHILD=1 DETAIL=65533 REASON=PROFILE_MISMATCH"));
  probeCommand("CMD TARGET=01.01 ACTION=MEASURE_RESET TX=B00005");
  cycle(sensor);
  CHECK(sensor.controls == 0);
  CHECK(probeSaw("ACK COMMAND=MEASURE_RESET STATE=REJECTED TX=B00005 NODE=1 CHILD=1 DETAIL=65533 "
                 "REASON=PROFILE_MISMATCH"));
  // IDLE vale para os dois perfis.
  probeCommand("CMD TARGET=01.03 ACTION=CONFIG TX=B00006 MODE=IDLE");
  cycle(mic);
  CHECK(mic.gotConfig && mic.lastConfig.target_mode == ACOUSTIC_MODE_IDLE);
  CHECK(probeSaw("status=APPLIED mode=IDLE rate_req_hz=48000.00"));

  // Ações acústicas: reinício da medição e assinatura de referência.
  probeCommand("CMD TARGET=01.03 ACTION=MEASURE_RESET TX=B00007");
  cycle(mic);
  CHECK(mic.controls == 1 && mic.lastControl.action == ACOUSTIC_CTRL_MEASURE_RESET);
  CHECK(probeSaw("ACK COMMAND=MEASURE_RESET STATE=ACCEPTED TX=B00007 NODE=1 CHILD=3"));
  CHECK(probeSaw("ACK COMMAND=MEASURE_RESET STATE=APPLIED TX=B00007 NODE=1 CHILD=3 DETAIL=1"));
  probeCommand("CMD TARGET=01.03 ACTION=BASELINE_LEARN TX=B00008 SECONDS=120");
  cycle(mic);
  CHECK(mic.controls == 2 && mic.lastControl.action == ACOUSTIC_CTRL_BASELINE_LEARN);
  CHECK(mic.lastControl.argument == 120);
  CHECK(probeSaw("ACK COMMAND=BASELINE_LEARN STATE=APPLIED TX=B00008 NODE=1 CHILD=3 DETAIL=2"));
  probeCommand("CMD TARGET=01.03 ACTION=BASELINE_LEARN TX=B00009 SECONDS=2");
  CHECK(probeSaw("ACK COMMAND=BASELINE_LEARN STATE=REJECTED TX=B00009 REASON=INVALID_FIELD"));
  CHECK(mic.controls == 2);
  probeCommand("CMD TARGET=01.03 ACTION=BASELINE_CLEAR TX=B0000A");
  cycle(mic);
  CHECK(mic.controls == 3 && mic.lastControl.action == ACOUSTIC_CTRL_BASELINE_CLEAR);
  CHECK(probeSaw("ACK COMMAND=BASELINE_CLEAR STATE=APPLIED TX=B0000A NODE=1 CHILD=3 DETAIL=3"));
  // Sem resposta do sensor, o Node encerra o pedido por tempo.
  probeCommand("CMD TARGET=01.03 ACTION=MEASURE_RESET TX=B0000B");
  asNode();
  wirelessLinkPoll();
  WiFiUDP::outbox.clear();       // o pedido se perde no ar
  deliverFrames(true);
  for (int i = 0; i < 4; ++i) {
    g_millis += 1000;
    cycle(mic, 3);
  }
  CHECK(probeSaw("ACK COMMAND=MEASURE_RESET STATE=REJECTED TX=B0000B NODE=1 CHILD=3 DETAIL=65535 "
                 "REASON=SENSOR_TIMEOUT"));
  asNode();
  wirelessLinkOnUnbound(MIC_UUID, 3);
  wirelessLinkOnUnbound(SENSOR_UUID, 1);
  mic.receive();
  sensor.receive();
  g_millis += 500;
  wirelessLinkPoll();
  deliverFrames(true);
  takeCanFrames();

  /* Tempos de resposta: PING/PONG no enlace e comando/confirmação na Probe. */
  CHECK(probeSaw(" rtt_ms=5.0 rtt_max_ms=5.0"));
  CHECK(probeSaw("ACK COMMAND=FFT STATE=APPLIED TX=A00002 NODE=1 CHILD=1 DETAIL=64 RTT_MS="));
  CHECK(probeSaw("state=OFFERING rssi=0 age_s=0 rx=0 auth_fail=0 replay=0 lost=0 stream=ON period_ms=1000 "
                 "rtt_ms=-1 rtt_max_ms=-1"));

  if (g_failures) {
    fprintf(stderr, "sim_wireless_link: %d falha(s)\n", g_failures);
    return 1;
  }
  fprintf(stderr, "sim_wireless_link: PASS\n");
  return 0;
}
