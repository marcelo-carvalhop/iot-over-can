#include "wireless_link.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <NimBLEDevice.h>
#include <esp_wifi.h>
#include <cmath>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if __has_include(<esp_random.h>)
#include <esp_random.h>
#else
#include <esp_system.h>
#endif

#include "can_ids.h"
#include "node_config.h"

#include "ioc_link.h"
#include "ioc_sha256.h"
#include "ioc_wdata.h"

// Estruturas do protocolo do sensor. O arquivo pertence ao firmware do Pico W
// e é a única definição do formato dos payloads internos. A inclusão é por
// caminho relativo de propósito: colocar Codigo/node-wifi no caminho de busca
// faria o lwipopts.h do Pico substituir o do ESP-IDF.
extern "C" {
#include "../../node-wifi/edge_protocol_definitions.h"
}

extern ACAN2515 can;

#ifndef IOT_LINK_MASTER_KEY_HEX
#define IOT_LINK_MASTER_KEY_HEX ""
#endif

namespace {

constexpr uint8_t  MAX_LINKS = 4;               // igual a MAX_BINDINGS em wireless_discovery.cpp
constexpr uint8_t  AP_MAX_STATIONS = 4;
constexpr uint16_t MAX_DATAGRAM = 1472;
constexpr uint16_t MAX_FFT_BINS = 256;
constexpr uint32_t OFFER_ROTATE_MS = 400;       // troca do sensor anunciado quando há mais de um
constexpr uint16_t OFFER_ADV_INTERVAL = 160;    // 100 ms em unidades de 0,625 ms
constexpr uint32_t PING_INTERVAL_MS = NET_LIVENESS_INTERVAL_MS;
constexpr uint32_t SESSION_TIMEOUT_MS = NET_SESSION_TIMEOUT_MS;
constexpr uint32_t HANDSHAKE_TIMEOUT_MS = 5000;
constexpr uint32_t LINK_REPORT_MS = 5000;
constexpr uint32_t APPLY_TIMEOUT_MS = 3000;
constexpr uint32_t FFT_TIMEOUT_MS = 5000;
constexpr uint16_t STREAM_PERIOD_DEFAULT_MS = 1000;
constexpr uint16_t STREAM_PERIOD_MIN_MS = 200;
constexpr uint16_t STREAM_PERIOD_MAX_MS = 10000;
constexpr uint8_t  TX_QUEUE_LEN = 8;
// O buffer de transmissão do MCP2515 é compartilhado com eleição, liveness e
// associação, e é FIFO. O plano de dados só acrescenta quadros enquanto ele
// está quase vazio, para que um quadro de controle nunca espere atrás de uma
// rajada de dados nem seja descartado por falta de espaço.
constexpr uint8_t  TX_DRIVER_WATERMARK = 4;
// Pausa após cada bloco de espectro: a Probe leva dezenas de milissegundos
// para escrever a linha correspondente na serial.
constexpr uint32_t SPECTRUM_GAP_MS = 40;
constexpr uint32_t AP_STOP_DELAY_MS = 400;      // tempo para o CMD_RELEASE sair antes de desligar o AP
constexpr uint32_t HELLO_GUARD_MS = 1000;       // um handshake recente não é substituído por outro HELLO
constexpr uint16_t ACK_DETAIL_TIMEOUT = 0xFFFF;
constexpr uint16_t ACK_DETAIL_CONFIG_UNKNOWN = 0xFFFE;
constexpr uint8_t  MAX_RX_DATAGRAMS_PER_POLL = 4;
constexpr uint8_t  PROBE_PENDING_LEN = 12;

/* ===================================================================== */
/* Node funcional                                                        */
/* ===================================================================== */

struct PendingHandshake {
  bool active = false;
  uint8_t sensorNonce[IOC_LINK_NONCE_LEN] = {0};
  uint8_t nodeNonce[IOC_LINK_NONCE_LEN] = {0};
  uint8_t sessionKey[IOC_LINK_KEY_LEN] = {0};
  IPAddress peerIp;
  uint16_t peerPort = 0;
  uint32_t startedMs = 0;
};

struct SensorConfig {
  bool known = false;
  uint8_t mode = 0;
  uint8_t windowType = 0;
  uint16_t windowSize = 0;
  float rateReqHz = 0.0f;
  float rateEffHz = 0.0f;
  float stalta = 0.0f;
  float gain = 0.0f;
};

struct Link {
  bool used = false;
  uint64_t uuid = 0;
  uint8_t childId = 0;
  uint8_t devKey[IOC_LINK_KEY_LEN] = {0};
  uint32_t offerNonce = 0;

  // Sessão autenticada em vigor.
  bool secure = false;
  uint8_t sessionKey[IOC_LINK_KEY_LEN] = {0};
  IPAddress peerIp;
  uint16_t peerPort = 0;
  uint32_t txCounter = 0;
  uint32_t rxLastCounter = 0;
  uint32_t lastRxMs = 0;
  uint32_t lastPingMs = 0;
  uint32_t sessionStartMs = 0;
  uint32_t configCounter = 0;
  uint8_t sessionSensorNonce[IOC_LINK_NONCE_LEN] = {0}; // nonce do HELLO que originou a sessão

  // Um HELLO recebido durante uma sessão não a derruba: a troca só ocorre
  // quando o CONFIRM do novo handshake é autenticado.
  PendingHandshake pending;

  // Contadores publicados em WIRELESS_LINK.
  uint16_t rxDatagrams = 0;
  uint16_t authFailures = 0;
  uint16_t replayDrops = 0;
  uint16_t lostDatagrams = 0;
  bool haveSeq = false;
  uint16_t lastSeq = 0;
  uint32_t lastLinkReportMs = 0;
  uint8_t lastReportedState = 0xFF;

  // Encaminhamento ao CAN.
  bool streamEnabled = true;
  bool streamOnce = false;
  uint16_t streamPeriodMs = STREAM_PERIOD_DEFAULT_MS;
  uint32_t lastForwardMs = 0;

  // Pedido de espectro em andamento.
  uint16_t fftBinsWanted = 0;
  uint8_t fftSeq = 0;
  uint32_t fftRequestedMs = 0;
  uint8_t spectrumId = 0;

  // Configuração conhecida do sensor e cópia em edição (SET ... / APPLY).
  SensorConfig current;
  SensorConfig staged;
  bool stagedDirty = false;
  bool applyPending = false;
  uint8_t applySeq = 0;
  uint32_t applySentMs = 0;
  bool clearPending = false;
  uint8_t clearSeq = 0;
  uint32_t clearSentMs = 0;
};

struct TxTransfer {
  uint8_t child = 0;
  uint8_t kind = 0;
  uint8_t transfer = 0;
  uint8_t len = 0;
  uint8_t next = 0;
  uint8_t data[IOC_WD_MAX_PAYLOAD] = {0};
};

Link links[MAX_LINKS];
uint8_t masterKey[IOC_LINK_KEY_LEN] = {0};
uint8_t networkKey[IOC_LINK_KEY_LEN] = {0};
bool haveKey = false;
bool apRunning = false;
bool apStopPending = false;
uint32_t apStopAtMs = 0;
uint32_t txNextTransferAtMs = 0;
bool offerAdvertising = false;
uint8_t offerCursor = 0;
uint32_t lastOfferRotateMs = 0;
WiFiUDP udp;
uint8_t rxDatagram[MAX_DATAGRAM];
uint8_t txDatagram[160];
float fftScratch[MAX_FFT_BINS];
uint16_t fftReduced[MAX_FFT_BINS];

TxTransfer txQueue[TX_QUEUE_LEN];
uint8_t txHead = 0;
uint8_t txCount = 0;
uint8_t txTransferCounter = 0;
uint16_t txDropped = 0;

void fillRandom(uint8_t* out, size_t len) {
  esp_fill_random(out, len);
}

Link* findLinkByUuid(uint64_t uuid) {
  for (uint8_t i = 0; i < MAX_LINKS; ++i) {
    if (links[i].used && links[i].uuid == uuid) return &links[i];
  }
  return nullptr;
}

Link* findLinkByChild(uint8_t childId) {
  for (uint8_t i = 0; i < MAX_LINKS; ++i) {
    if (links[i].used && links[i].childId == childId) return &links[i];
  }
  return nullptr;
}

Link* findLinkByPeer(const IPAddress& ip, uint16_t port) {
  for (uint8_t i = 0; i < MAX_LINKS; ++i) {
    if (links[i].used && links[i].secure && links[i].peerPort == port && links[i].peerIp == ip) {
      return &links[i];
    }
  }
  return nullptr;
}

uint8_t usedLinkCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_LINKS; ++i) if (links[i].used) ++n;
  return n;
}

uint8_t linkState(const Link& link) {
  if (!haveKey) return IOC_WD_LINK_NO_KEY;
  if (link.secure) return IOC_WD_LINK_SECURE;
  if (link.pending.active) return IOC_WD_LINK_HANDSHAKE;
  return IOC_WD_LINK_OFFERING;
}

/* ------------------------------------------------------------ fila CAN */

// As transferências são enfileiradas e drenadas em wirelessLinkPoll(): o
// buffer de transmissão do MCP2515 é pequeno e um espectro ocupa dezenas de
// quadros. A fila também impede que dados de sensores atrasem o laço principal.
bool queueTransfer(uint8_t child, uint8_t kind, const uint8_t* payload, size_t len) {
  if (ioc_wd_segment_count(len) == 0) return false;
  if (txCount >= TX_QUEUE_LEN) {
    if (txDropped < 0xFFFF) ++txDropped;
    return false;
  }
  TxTransfer& slot = txQueue[(txHead + txCount) % TX_QUEUE_LEN];
  slot.child = child;
  slot.kind = kind;
  slot.transfer = static_cast<uint8_t>(txTransferCounter++ & 0x07u);
  slot.len = static_cast<uint8_t>(len);
  slot.next = 0;
  memcpy(slot.data, payload, len);
  ++txCount;
  return true;
}

// Descarta o que ainda não começou a ser transmitido para um filho removido,
// evitando que telemetria atrasada chegue depois do UNBOUND.
void purgeQueuedTransfers(uint8_t child) {
  uint8_t kept = 0;
  for (uint8_t i = 0; i < txCount; ++i) {
    TxTransfer& slot = txQueue[(txHead + i) % TX_QUEUE_LEN];
    if (slot.child == child && slot.next == 0) continue;
    if (kept != i) txQueue[(txHead + kept) % TX_QUEUE_LEN] = slot;
    ++kept;
  }
  txCount = kept;
}

void drainTxQueue() {
  const uint32_t now = millis();
  while (txCount > 0) {
    TxTransfer& slot = txQueue[txHead];
    if (slot.next == 0 && static_cast<int32_t>(now - txNextTransferAtMs) < 0) return;
    const uint8_t segments = ioc_wd_segment_count(slot.len);
    while (slot.next < segments) {
      if (can.transmitBufferCount(0) >= TX_DRIVER_WATERMARK) return; // continua no próximo ciclo
      CANMessage msg;
      msg.id = CAN_ID_WIRELESS_DATA_BASE + NODE_ID;
      msg.len = IOC_WD_FRAME_LEN;
      if (!ioc_wd_build_segment(slot.child, slot.kind, slot.transfer, slot.data, slot.len,
                                slot.next, msg.data)) {
        slot.next = segments; // nunca deveria ocorrer; descarta a transferência
        break;
      }
      if (!can.tryToSend(msg)) return;
      ++slot.next;
    }
    if (slot.kind == IOC_WD_KIND_SPECTRUM) txNextTransferAtMs = now + SPECTRUM_GAP_MS;
    txHead = static_cast<uint8_t>((txHead + 1u) % TX_QUEUE_LEN);
    --txCount;
  }
}

void publishAck(uint8_t child, uint8_t seq, uint8_t opcode, uint8_t status, uint16_t detail = 0) {
  ioc_wd_ack ack = {seq, opcode, status, detail};
  uint8_t raw[IOC_WD_ACK_LEN];
  ioc_wd_ack_encode(&ack, raw);
  queueTransfer(child, IOC_WD_KIND_CMD_ACK, raw, sizeof(raw));
}

int8_t stationRssi() {
  // O RSSI Wi-Fi só é atribuído sem ambiguidade quando há uma única estação.
  wifi_sta_list_t list;
  memset(&list, 0, sizeof(list));
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || list.num != 1) return 0;
  return static_cast<int8_t>(list.sta[0].rssi);
}

void publishLink(Link& link, bool force = false) {
  const uint8_t state = linkState(link);
  const uint32_t now = millis();
  if (!force && state == link.lastReportedState && now - link.lastLinkReportMs < LINK_REPORT_MS) return;
  link.lastReportedState = state;
  link.lastLinkReportMs = now;

  ioc_wd_link report;
  report.state = state;
  report.wifi_rssi = link.secure ? stationRssi() : 0;
  const uint32_t ageS = link.secure ? (now - link.sessionStartMs) / 1000u : 0u;
  report.session_age_s = ageS > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(ageS);
  report.rx_datagrams = link.rxDatagrams;
  report.auth_failures = link.authFailures;
  report.replay_drops = link.replayDrops;
  report.lost_datagrams = link.lostDatagrams;
  report.stream_enabled = link.streamEnabled ? 1 : 0;
  report.stream_period_ms = link.streamPeriodMs;
  uint8_t raw[IOC_WD_LINK_LEN];
  ioc_wd_link_encode(&report, raw);
  queueTransfer(link.childId, IOC_WD_KIND_LINK, raw, sizeof(raw));
}

void publishConfig(const Link& link, uint8_t status) {
  if (!link.current.known) return;
  ioc_wd_config cfg;
  cfg.mode = link.current.mode;
  cfg.window_type = link.current.windowType;
  cfg.window_size = link.current.windowSize;
  cfg.rate_req_hz = link.current.rateReqHz;
  cfg.rate_eff_hz = link.current.rateEffHz;
  cfg.stalta = link.current.stalta;
  cfg.gain = link.current.gain;
  cfg.status = status;
  uint8_t raw[IOC_WD_CONFIG_LEN];
  ioc_wd_config_encode(&cfg, raw);
  queueTransfer(link.childId, IOC_WD_KIND_CONFIG, raw, sizeof(raw));
}

/* ------------------------------------------------------- rádio do Node */

void startAccessPoint() {
  apStopPending = false; // um novo vínculo cancela o desligamento agendado
  if (apRunning || !haveKey) return;
  char ssid[IOC_LINK_SSID_LEN];
  char psk[IOC_LINK_PSK_LEN];
  ioc_link_wifi_ssid(NODE_ID, ssid);
  ioc_link_wifi_psk(networkKey, NODE_ID, psk);
  WiFi.persistent(false); // SSID e senha derivados não são gravados na NVS
  WiFi.mode(WIFI_AP);
  const bool ok = WiFi.softAP(ssid, psk, ioc_link_wifi_channel(NODE_ID), 0, AP_MAX_STATIONS);
  ioc_secure_zero(psk, sizeof(psk));
  if (!ok) {
    Serial.printf("[NODE %u] [WLINK] AP=ERROR\n", NODE_ID);
    return;
  }
  udp.begin(IOC_LINK_UDP_PORT);
  apRunning = true;
  Serial.printf("[NODE %u] [WLINK] AP=ON ssid=%s channel=%u\n", NODE_ID, ssid,
                static_cast<unsigned>(ioc_link_wifi_channel(NODE_ID)));
}

void stopAccessPoint() {
  apStopPending = false;
  if (!apRunning) return;
  udp.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  apRunning = false;
  Serial.printf("[NODE %u] [WLINK] AP=OFF\n", NODE_ID);
}

void stopOfferAdvertising() {
  if (!offerAdvertising) return;
  NimBLEDevice::getAdvertising()->stop();
  offerAdvertising = false;
}

// Anuncia a oferta de um sensor que ainda não tem sessão. Havendo mais de um,
// alterna entre eles.
void updateOfferAdvertising() {
  if (!haveKey) return;
  const uint32_t now = millis();
  bool anyWaiting = false;
  for (uint8_t i = 0; i < MAX_LINKS; ++i) {
    if (links[i].used && !links[i].secure) anyWaiting = true;
  }
  if (!anyWaiting) {
    stopOfferAdvertising();
    return;
  }
  if (offerAdvertising && now - lastOfferRotateMs < OFFER_ROTATE_MS) return;

  Link* target = nullptr;
  for (uint8_t step = 0; step < MAX_LINKS; ++step) {
    Link& candidate = links[(offerCursor + step) % MAX_LINKS];
    if (candidate.used && !candidate.secure) {
      target = &candidate;
      offerCursor = static_cast<uint8_t>((offerCursor + step + 1u) % MAX_LINKS);
      break;
    }
  }
  if (!target) {
    stopOfferAdvertising();
    return;
  }

  ioc_offer_t offer;
  offer.uuid = target->uuid;
  offer.node_id = NODE_ID;
  offer.channel = ioc_link_wifi_channel(NODE_ID);
  offer.nonce = ++target->offerNonce;
  uint8_t mfg[IOC_OFFER_MFG_LEN];
  ioc_offer_build(target->devKey, &offer, mfg);

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData data;
  data.setManufacturerData(std::string(reinterpret_cast<const char*>(mfg), sizeof(mfg)));
  if (offerAdvertising) adv->stop();
  adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  adv->setMinInterval(OFFER_ADV_INTERVAL);
  adv->setMaxInterval(OFFER_ADV_INTERVAL);
  adv->setAdvertisementData(data);
  offerAdvertising = adv->start();
  lastOfferRotateMs = now;
}

/* ----------------------------------------------------- envio ao sensor */

bool sendEnvelope(const uint8_t* key, uint8_t type, uint32_t counter, const void* payload,
                  size_t len, const IPAddress& ip, uint16_t port) {
  const size_t total = ioc_env_seal(key, type, counter, static_cast<const uint8_t*>(payload), len,
                                    txDatagram, sizeof(txDatagram));
  if (total == 0) return false;
  if (!udp.beginPacket(ip, port)) return false;
  udp.write(txDatagram, total);
  return udp.endPacket() == 1;
}

bool sendToSensor(Link& link, const void* inner, size_t len) {
  if (!link.secure || link.txCounter == 0xFFFFFFFFu) return false;
  return sendEnvelope(link.sessionKey, IOC_MSG_DATA_DOWN, ++link.txCounter, inner, len,
                      link.peerIp, link.peerPort);
}

void sendPing(Link& link) {
  Payload_Heartbeat ping;
  memset(&ping, 0, sizeof(ping));
  ping.magic_header = NET_MAGIC_HEADER;
  ping.cmd_type = CMD_PING;
  ping.request_counter = link.txCounter + 1u;
  sendToSensor(link, &ping, sizeof(ping));
  link.lastPingMs = millis();
}

void sendSimpleCommand(Link& link, uint8_t cmdType) {
  uint8_t raw[3];
  const uint16_t magic = NET_MAGIC_HEADER;
  memcpy(&raw[0], &magic, 2);
  raw[2] = cmdType;
  sendToSensor(link, raw, sizeof(raw));
}

void dropSession(Link& link) {
  ioc_secure_zero(link.sessionKey, sizeof(link.sessionKey));
  link.secure = false;
  link.txCounter = 0;
  link.rxLastCounter = 0;
  link.peerPort = 0;
  link.haveSeq = false;
  link.fftBinsWanted = 0;
  link.applyPending = false;
  link.clearPending = false;
  link.staged = link.current;
  link.stagedDirty = false;
}

/* ------------------------------------------------ dados vindos do sensor */

const char* modeName(uint8_t mode) {
  switch (mode) {
    case FSM_MODE_IDLE: return "IDLE";
    case FSM_MODE_ROTATING_MACH: return "ROTATING";
    case FSM_MODE_STRUCTURAL: return "STRUCTURAL";
    case FSM_MODE_SEISMIC_STALTA: return "SEISMIC";
    default: return "UNKNOWN";
  }
}

void forwardSpectrum(Link& link, const uint8_t* fftBytes, uint16_t bins, float sampleRateHz) {
  if (bins > MAX_FFT_BINS) bins = MAX_FFT_BINS;
  memcpy(fftScratch, fftBytes, static_cast<size_t>(bins) * sizeof(float));

  uint16_t outBins = link.fftBinsWanted;
  if (outBins > bins) outBins = bins;
  float scale = 0.0f;
  ioc_wd_spectrum_reduce(fftScratch, bins, fftReduced, outBins, &scale);

  const uint8_t spectrumId = ++link.spectrumId;
  uint8_t raw[IOC_WD_MAX_PAYLOAD];
  bool complete = true;
  for (uint16_t offset = 0; offset < outBins; offset += IOC_WD_SPECTRUM_CHUNK_BINS) {
    ioc_wd_spectrum_header hdr;
    hdr.spectrum_id = spectrumId;
    hdr.total_bins = outBins;
    hdr.offset = offset;
    const uint16_t remaining = static_cast<uint16_t>(outBins - offset);
    hdr.count = static_cast<uint8_t>(remaining > IOC_WD_SPECTRUM_CHUNK_BINS ? IOC_WD_SPECTRUM_CHUNK_BINS : remaining);
    hdr.scale = scale;
    hdr.sample_rate_hz = sampleRateHz;
    hdr.fft_size = static_cast<uint16_t>(outBins * 2u); // resolução equivalente após a redução
    hdr.window_type = link.current.windowType;
    const size_t n = ioc_wd_spectrum_encode(&hdr, &fftReduced[offset], raw, sizeof(raw));
    if (n == 0 || !queueTransfer(link.childId, IOC_WD_KIND_SPECTRUM, raw, n)) {
      complete = false;
      break;
    }
  }
  publishAck(link.childId, link.fftSeq, IOC_WD_OP_FFT,
             complete ? IOC_WD_ACK_APPLIED : IOC_WD_ACK_REJECTED, outBins);
  link.fftBinsWanted = 0;
}

void handleTelemetry(Link& link, const uint8_t* inner, size_t len) {
  Payload_TelemetryStream t;
  if (len < sizeof(t)) return;
  memcpy(&t, inner, sizeof(t));
  const size_t fftBytes = len - sizeof(t);
  if (fftBytes != static_cast<size_t>(t.fft_bins_count) * sizeof(float)) return; // tamanho incoerente

  if (link.haveSeq) {
    const uint16_t gap = static_cast<uint16_t>(t.seq_num - link.lastSeq);
    if (gap > 1u && gap < 0x8000u) {
      const uint32_t lost = static_cast<uint32_t>(link.lostDatagrams) + (gap - 1u);
      link.lostDatagrams = lost > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(lost);
    }
  }
  link.haveSeq = true;
  link.lastSeq = t.seq_num;

  const uint32_t now = millis();
  if (link.streamOnce || (link.streamEnabled && now - link.lastForwardMs >= link.streamPeriodMs)) {
    ioc_wd_telemetry out;
    out.seq = t.seq_num;
    out.mode = t.fsm_mode;
    out.acquisition = t.acquisition_mode;
    out.axis_mask = t.axis_mask;
    out.flags = static_cast<uint8_t>((t.fft_valid ? IOC_WD_TEL_FLAG_FFT_VALID : 0) |
                                     (t.clipping_detected ? IOC_WD_TEL_FLAG_CLIPPING : 0) |
                                     (t.seismic_triggered ? IOC_WD_TEL_FLAG_STALTA : 0));
    out.window_size = t.window_size;
    out.rate_req_hz = t.sample_rate_req_hz;
    out.rate_eff_hz = t.sample_rate_eff_hz;
    out.rms = t.rms_ac;
    out.kurtosis = t.kurtosis;
    out.crest = t.crest_factor;
    out.peak_hz = t.peak_freq_hz;
    out.peak_amp = t.peak_amplitude;
    out.entropy = t.entropy;
    out.ppv_mm_s = t.ppv_max_mm_s;
    out.battery_pct = t.battery_pct;
    out.battery_mv = t.battery_mv;
    out.dtc_count = t.dtc_count;
    out.dtc_code = t.dtc_active_code;
    uint8_t raw[IOC_WD_TELEMETRY_LEN];
    ioc_wd_telemetry_encode(&out, raw);
    if (queueTransfer(link.childId, IOC_WD_KIND_TELEMETRY, raw, sizeof(raw))) {
      link.lastForwardMs = now;
      link.streamOnce = false;
    }
  }

  if (link.fftBinsWanted > 0) {
    if (t.fft_valid && t.fft_bins_count > 0) {
      forwardSpectrum(link, inner + sizeof(t), t.fft_bins_count, t.sample_rate_eff_hz);
    } else if (t.fsm_mode == FSM_MODE_SEISMIC_STALTA || t.fsm_mode == FSM_MODE_IDLE) {
      // Modos sem FFT: o pedido não pode ser atendido.
      publishAck(link.childId, link.fftSeq, IOC_WD_OP_FFT, IOC_WD_ACK_REJECTED, 0);
      link.fftBinsWanted = 0;
    }
  }
}

void handleConfigMenu(Link& link, const uint8_t* inner, size_t len) {
  Payload_ConfigMenu menu;
  if (len != sizeof(menu)) return;
  memcpy(&menu, inner, sizeof(menu));
  link.current.known = true;
  link.current.mode = menu.current_fsm_mode;
  link.current.windowType = menu.current_window_type;
  link.current.windowSize = menu.current_buffer_size;
  link.current.rateEffHz = menu.current_sample_rate_hz;
  // O menu informa apenas a taxa efetiva; sem edição em curso ela é também a solicitada.
  if (!link.stagedDirty) link.current.rateReqHz = menu.current_sample_rate_hz;
  link.current.stalta = menu.current_stalta_thresh;
  link.current.gain = menu.current_calib_gain;
  if (!link.stagedDirty) link.staged = link.current;
  publishConfig(link, 0);
}

void handleConfigAck(Link& link, const uint8_t* inner, size_t len) {
  Payload_ConfigAck ack;
  if (len != sizeof(ack)) return;
  memcpy(&ack, inner, sizeof(ack));
  if (ack.status == 0) return; // enfileirada no sensor; a confirmação final vem em seguida

  if (ack.status == 1) {
    link.current = link.staged;
    link.current.known = true;
    link.current.mode = ack.target_mode;
    link.current.windowSize = ack.effective_window_size;
    link.current.rateReqHz = ack.requested_sample_rate_hz;
    link.current.rateEffHz = ack.effective_sample_rate_hz;
    link.staged = link.current;
  } else {
    link.staged = link.current; // descarta a edição recusada
  }
  link.stagedDirty = false;
  if (link.applyPending) {
    publishAck(link.childId, link.applySeq, IOC_WD_OP_APPLY,
               ack.status == 1 ? IOC_WD_ACK_APPLIED : IOC_WD_ACK_REJECTED, ack.status);
    link.applyPending = false;
  }
  publishConfig(link, ack.status == 1 ? 1 : 2);
}

void handleUrgentDtc(Link& link, const uint8_t* inner, size_t len) {
  DTC_Record rec;
  if (len != 3u + sizeof(rec)) return;
  memcpy(&rec, inner + 3, sizeof(rec));
  ioc_wd_dtc dtc = {rec.dtc_code, rec.symptom, rec.severity, rec.freeze.timestamp_ms};
  uint8_t raw[IOC_WD_DTC_LEN];
  ioc_wd_dtc_encode(&dtc, raw);
  queueTransfer(link.childId, IOC_WD_KIND_DTC, raw, sizeof(raw));
}

void handleSensorPayload(Link& link, const uint8_t* inner, size_t len) {
  if (len < 3) return;
  uint16_t magic;
  memcpy(&magic, inner, 2);
  if (magic != NET_MAGIC_HEADER) return;

  switch (inner[2]) {
    case CMD_TELEMETRY_STREAM:
      handleTelemetry(link, inner, len);
      break;
    case CMD_CONFIG_MENU:
      handleConfigMenu(link, inner, len);
      break;
    case CMD_ACK_CONFIG:
      handleConfigAck(link, inner, len);
      break;
    case CMD_DTC_SNAPSHOT:
      if (link.clearPending) {
        publishAck(link.childId, link.clearSeq, IOC_WD_OP_DTC_CLEAR, IOC_WD_ACK_APPLIED, 0);
        link.clearPending = false;
      }
      break;
    case CMD_URGENT_DTC_ALARM:
      handleUrgentDtc(link, inner, len);
      break;
    case CMD_PONG:
    default:
      break;
  }
}

/* --------------------------------------------------------- handshake */

void handleHello(const uint8_t* datagram, size_t len, const ioc_env_view& view,
                 const IPAddress& ip, uint16_t port) {
  ioc_hello_t hello;
  if (!ioc_hello_decode(view.payload, view.payload_len, &hello)) return;

  // Só existe chave para sensores que o operador associou a este Node. Um
  // HELLO de UUID desconhecido é ignorado sem resposta.
  Link* link = findLinkByUuid(hello.uuid);
  if (!link) return;
  if (!ioc_env_verify(link->devKey, datagram, len)) {
    if (link->authFailures < 0xFFFF) ++link->authFailures;
    return;
  }
  if (hello.protocol != IOC_LINK_VERSION) return;

  // O HELLO não carrega nada que o Node possa conferir quanto à atualidade,
  // então uma cópia capturada continua autêntica. Três regras limitam o que
  // uma repetição consegue fazer:
  //  - o HELLO que originou a sessão em vigor é ignorado;
  //  - o mesmo HELLO recebido de novo reenvia o mesmo CHALLENGE;
  //  - um handshake recém-iniciado não é substituído por outro HELLO.
  PendingHandshake& pending = link->pending;
  if (link->secure && ioc_ct_equal(hello.sensor_nonce, link->sessionSensorNonce, IOC_LINK_NONCE_LEN)) {
    if (link->replayDrops < 0xFFFF) ++link->replayDrops;
    return;
  }
  const bool sameHello = pending.active &&
                         ioc_ct_equal(hello.sensor_nonce, pending.sensorNonce, IOC_LINK_NONCE_LEN);
  if (pending.active && !sameHello && millis() - pending.startedMs < HELLO_GUARD_MS) return;

  if (!sameHello) {
    pending.active = true;
    pending.startedMs = millis();
    memcpy(pending.sensorNonce, hello.sensor_nonce, IOC_LINK_NONCE_LEN);
    fillRandom(pending.nodeNonce, IOC_LINK_NONCE_LEN);
    ioc_link_derive_session_key(link->devKey, link->uuid, NODE_ID, pending.sensorNonce,
                                pending.nodeNonce, pending.sessionKey);
  }
  pending.peerIp = ip;
  pending.peerPort = port;

  ioc_challenge_t challenge;
  challenge.node_id = NODE_ID;
  challenge.child_id = link->childId;
  memcpy(challenge.node_nonce, pending.nodeNonce, IOC_LINK_NONCE_LEN);
  memcpy(challenge.sensor_nonce, pending.sensorNonce, IOC_LINK_NONCE_LEN);
  uint8_t raw[IOC_CHALLENGE_LEN];
  ioc_challenge_encode(&challenge, raw);
  sendEnvelope(link->devKey, IOC_MSG_CHALLENGE, 0, raw, sizeof(raw), ip, port);
  publishLink(*link);
}

// Promove o handshake pendente a sessão em vigor. `counter` é o contador do
// datagrama que acabou de ser autenticado com a nova chave.
void promotePending(Link& link, const IPAddress& ip, uint16_t port, uint32_t counter) {
  memcpy(link.sessionKey, link.pending.sessionKey, IOC_LINK_KEY_LEN);
  memcpy(link.sessionSensorNonce, link.pending.sensorNonce, IOC_LINK_NONCE_LEN);
  ioc_secure_zero(link.pending.sessionKey, IOC_LINK_KEY_LEN);
  link.pending.active = false;
  link.secure = true;
  link.peerIp = ip;
  link.peerPort = port;
  link.txCounter = 0;
  link.rxLastCounter = counter;
  link.lastRxMs = millis();
  link.lastPingMs = link.lastRxMs;
  link.sessionStartMs = link.lastRxMs;
  link.haveSeq = false;
  link.lastForwardMs = 0;
  link.applyPending = false;
  link.clearPending = false;
  link.fftBinsWanted = 0;
  // Uma sessão nova pode ser um sensor que reiniciou com outra configuração:
  // o que se sabia deixa de valer até o menu chegar.
  link.current.known = false;
  link.staged = link.current;
  link.stagedDirty = false;
  // O sensor envia o menu de configuração após o CONFIRM, mas um datagrama
  // UDP pode se perder; o pedido explícito cobre esse caso.
  sendSimpleCommand(link, CMD_GET_CONFIG_MENU);
  Serial.printf("[NODE %u] [WLINK] child=%u uuid=0x%016llX SECURE\n", NODE_ID,
                static_cast<unsigned>(link.childId), static_cast<unsigned long long>(link.uuid));
  publishLink(link, true);
}

void handleConfirm(const uint8_t* datagram, size_t len, const ioc_env_view& view,
                   const IPAddress& ip, uint16_t port) {
  ioc_confirm_t confirm;
  if (!ioc_confirm_decode(view.payload, view.payload_len, &confirm)) return;

  for (uint8_t i = 0; i < MAX_LINKS; ++i) {
    Link& link = links[i];
    if (!link.used || !link.pending.active) continue;
    if (!ioc_env_verify(link.pending.sessionKey, datagram, len)) continue;
    if (!ioc_ct_equal(confirm.node_nonce, link.pending.nodeNonce, IOC_LINK_NONCE_LEN) ||
        !ioc_ct_equal(confirm.sensor_nonce, link.pending.sensorNonce, IOC_LINK_NONCE_LEN)) {
      continue;
    }
    // O sensor provou conhecer K_dev e estar respondendo ao nonce gerado
    // agora por este Node.
    promotePending(link, ip, port, view.counter);
    return;
  }
}

void handleDataUp(const uint8_t* datagram, size_t len, const ioc_env_view& view,
                  const IPAddress& ip, uint16_t port) {
  Link* link = findLinkByPeer(ip, port);
  if (!link) {
    // O CONFIRM pode ter se perdido. Dados autenticados com a chave do
    // handshake pendente provam o mesmo que ele e concluem a troca.
    for (uint8_t i = 0; i < MAX_LINKS; ++i) {
      Link& candidate = links[i];
      if (!candidate.used || !candidate.pending.active) continue;
      if (candidate.pending.peerPort != port || !(candidate.pending.peerIp == ip)) continue;
      if (!ioc_env_verify(candidate.pending.sessionKey, datagram, len)) continue;
      promotePending(candidate, ip, port, view.counter);
      if (candidate.rxDatagrams < 0xFFFF) ++candidate.rxDatagrams;
      handleSensorPayload(candidate, view.payload, view.payload_len);
      return;
    }
    return;
  }
  if (!ioc_env_verify(link->sessionKey, datagram, len)) {
    // O sensor pode ter refeito o handshake (reinício, CONFIRM perdido) do
    // mesmo endereço enquanto o Node ainda guarda a sessão anterior.
    if (link->pending.active && ioc_env_verify(link->pending.sessionKey, datagram, len)) {
      promotePending(*link, ip, port, view.counter);
      if (link->rxDatagrams < 0xFFFF) ++link->rxDatagrams;
      handleSensorPayload(*link, view.payload, view.payload_len);
      return;
    }
    if (link->authFailures < 0xFFFF) ++link->authFailures;
    return;
  }
  if (!ioc_counter_accept(&link->rxLastCounter, view.counter)) {
    if (link->replayDrops < 0xFFFF) ++link->replayDrops;
    return;
  }
  link->lastRxMs = millis();
  if (link->rxDatagrams < 0xFFFF) ++link->rxDatagrams;
  handleSensorPayload(*link, view.payload, view.payload_len);
}

void receiveDatagrams() {
  if (!apRunning) return;
  for (uint8_t n = 0; n < MAX_RX_DATAGRAMS_PER_POLL; ++n) {
    const int size = udp.parsePacket();
    if (size <= 0) return;
    if (size > static_cast<int>(sizeof(rxDatagram))) {
      udp.flush();
      continue;
    }
    const int got = udp.read(rxDatagram, sizeof(rxDatagram));
    if (got < static_cast<int>(IOC_ENV_OVERHEAD)) continue;
    const IPAddress ip = udp.remoteIP();
    const uint16_t port = udp.remotePort();

    ioc_env_view view;
    if (!ioc_env_parse(rxDatagram, static_cast<size_t>(got), &view)) continue;
    switch (view.type) {
      case IOC_MSG_HELLO:   handleHello(rxDatagram, got, view, ip, port); break;
      case IOC_MSG_CONFIRM: handleConfirm(rxDatagram, got, view, ip, port); break;
      case IOC_MSG_DATA_UP: handleDataUp(rxDatagram, got, view, ip, port); break;
      default: break;
    }
  }
}

void maintainLinks() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < MAX_LINKS; ++i) {
    Link& link = links[i];
    if (!link.used) continue;

    if (link.pending.active && now - link.pending.startedMs > HANDSHAKE_TIMEOUT_MS) {
      ioc_secure_zero(link.pending.sessionKey, IOC_LINK_KEY_LEN);
      link.pending.active = false;
    }

    if (link.secure) {
      if (now - link.lastRxMs > SESSION_TIMEOUT_MS) {
        Serial.printf("[NODE %u] [WLINK] child=%u sessao expirada\n", NODE_ID,
                      static_cast<unsigned>(link.childId));
        dropSession(link);
        publishLink(link, true);
      } else if (now - link.lastPingMs >= PING_INTERVAL_MS) {
        sendPing(link);
      }
    }

    if (link.applyPending && now - link.applySentMs > APPLY_TIMEOUT_MS) {
      publishAck(link.childId, link.applySeq, IOC_WD_OP_APPLY, IOC_WD_ACK_REJECTED, ACK_DETAIL_TIMEOUT);
      link.applyPending = false;
      link.staged = link.current; // a edição sem resposta não acompanha o próximo APPLY
      link.stagedDirty = false;
    }
    if (link.clearPending && now - link.clearSentMs > APPLY_TIMEOUT_MS) {
      publishAck(link.childId, link.clearSeq, IOC_WD_OP_DTC_CLEAR, IOC_WD_ACK_REJECTED, ACK_DETAIL_TIMEOUT);
      link.clearPending = false;
    }
    if (link.fftBinsWanted > 0 && now - link.fftRequestedMs > FFT_TIMEOUT_MS) {
      publishAck(link.childId, link.fftSeq, IOC_WD_OP_FFT, IOC_WD_ACK_REJECTED, ACK_DETAIL_TIMEOUT);
      link.fftBinsWanted = 0;
    }

    publishLink(link);
  }
}

/* -------------------------------------------- comandos vindos do CAN */

void applyStagedConfig(Link& link, uint8_t seq) {
  if (!link.current.known || !link.staged.known) {
    // Sem a configuração atual do sensor não há base para montar a nova.
    sendSimpleCommand(link, CMD_GET_CONFIG_MENU);
    link.staged = link.current;
    link.stagedDirty = false;
    publishAck(link.childId, seq, IOC_WD_OP_APPLY, IOC_WD_ACK_REJECTED, ACK_DETAIL_CONFIG_UNKNOWN);
    return;
  }
  Payload_Configuration cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic_header = NET_MAGIC_HEADER;
  cfg.cmd_type = CMD_SET_CONFIG;
  cfg.request_counter = ++link.configCounter;
  cfg.target_mode = link.staged.mode;
  cfg.sample_rate_hz = link.staged.rateReqHz;
  cfg.window_size = link.staged.windowSize;
  cfg.window_type = link.staged.windowType;
  cfg.stalta_thresh = link.staged.stalta;
  cfg.calib_gain = link.staged.gain;
  if (!sendToSensor(link, &cfg, sizeof(cfg))) {
    link.staged = link.current;
    link.stagedDirty = false;
    publishAck(link.childId, seq, IOC_WD_OP_APPLY, IOC_WD_ACK_NO_LINK, 0);
    return;
  }
  link.applyPending = true;
  link.applySeq = seq;
  link.applySentMs = millis();
  publishAck(link.childId, seq, IOC_WD_OP_APPLY, IOC_WD_ACK_ACCEPTED, 0);
}

void handleDataCommand(const ioc_wd_command& cmd) {
  Link* link = findLinkByChild(cmd.child_id);
  if (!link) {
    publishAck(cmd.child_id & IOC_WD_MAX_CHILD, cmd.seq, cmd.opcode, IOC_WD_ACK_UNKNOWN, 0);
    return;
  }

  // Controle do encaminhamento e consulta de estado são locais ao Node e
  // funcionam mesmo sem sessão. O restante precisa do sensor autenticado.
  if (cmd.opcode == IOC_WD_OP_STREAM) {
    uint16_t period = ioc_wd_arg_u16(&cmd.arg[1]);
    if (period != 0) {
      if (period < STREAM_PERIOD_MIN_MS) period = STREAM_PERIOD_MIN_MS;
      if (period > STREAM_PERIOD_MAX_MS) period = STREAM_PERIOD_MAX_MS;
      link->streamPeriodMs = period;
    }
    if (cmd.arg[0] == 0) link->streamEnabled = false;
    else if (cmd.arg[0] == 1) link->streamEnabled = true;
    else if (cmd.arg[0] == 2) link->streamOnce = true;
    publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_APPLIED, link->streamPeriodMs);
    publishLink(*link, true);
    return;
  }
  if (cmd.opcode == IOC_WD_OP_STATUS) {
    if (link->secure && !link->current.known) sendSimpleCommand(*link, CMD_GET_CONFIG_MENU);
    publishLink(*link, true);
    publishConfig(*link, 0);
    publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_APPLIED, 0);
    return;
  }

  if (!link->secure) {
    publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_NO_LINK, 0);
    return;
  }

  const bool isSet = cmd.opcode >= IOC_WD_OP_SET_MODE && cmd.opcode <= IOC_WD_OP_SET_GAIN;
  if (isSet && !link->current.known) {
    sendSimpleCommand(*link, CMD_GET_CONFIG_MENU);
    publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_REJECTED, ACK_DETAIL_CONFIG_UNKNOWN);
    return;
  }

  switch (cmd.opcode) {
    case IOC_WD_OP_FFT: {
      uint16_t bins = ioc_wd_arg_u16(&cmd.arg[0]);
      if (bins < 16) bins = 16;
      if (bins > MAX_FFT_BINS) bins = MAX_FFT_BINS;
      link->fftBinsWanted = bins;
      link->fftSeq = cmd.seq;
      link->fftRequestedMs = millis();
      publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_ACCEPTED, bins);
      break;
    }
    case IOC_WD_OP_SET_MODE:
      link->staged.mode = cmd.arg[0];
      link->stagedDirty = true;
      publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_ACCEPTED, 0);
      break;
    case IOC_WD_OP_SET_RATE:
      link->staged.rateReqHz = ioc_wd_arg_f32(cmd.arg);
      link->stagedDirty = true;
      publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_ACCEPTED, 0);
      break;
    case IOC_WD_OP_SET_WINDOW: {
      const uint16_t size = ioc_wd_arg_u16(&cmd.arg[1]);
      if (cmd.arg[0] != 0xFF) link->staged.windowType = cmd.arg[0];
      if (size != 0) link->staged.windowSize = size;
      link->stagedDirty = true;
      publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_ACCEPTED, 0);
      break;
    }
    case IOC_WD_OP_SET_STALTA:
      link->staged.stalta = ioc_wd_arg_f32(cmd.arg);
      link->stagedDirty = true;
      publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_ACCEPTED, 0);
      break;
    case IOC_WD_OP_SET_GAIN:
      link->staged.gain = ioc_wd_arg_f32(cmd.arg);
      link->stagedDirty = true;
      publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_ACCEPTED, 0);
      break;
    case IOC_WD_OP_APPLY:
      applyStagedConfig(*link, cmd.seq);
      break;
    case IOC_WD_OP_DTC_CLEAR: {
      Payload_ClearDTC clear;
      memset(&clear, 0, sizeof(clear));
      clear.magic_header = NET_MAGIC_HEADER;
      clear.cmd_type = CMD_CLEAR_DTC;
      clear.request_counter = ++link->configCounter;
      if (sendToSensor(*link, &clear, sizeof(clear))) {
        link->clearPending = true;
        link->clearSeq = cmd.seq;
        link->clearSentMs = millis();
        publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_ACCEPTED, 0);
      } else {
        publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_NO_LINK, 0);
      }
      break;
    }
    default:
      publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_UNKNOWN, 0);
      break;
  }
}

/* ===================================================================== */
/* Probe 00                                                              */
/* ===================================================================== */

struct ProbePending {
  bool used = false;
  uint8_t seq = 0;
  uint8_t node = 0;
  uint8_t child = 0;
  bool report = false;       // false para os SET intermediários de um CONFIG
  char tx[12] = {0};
  char action[20] = {0};
};

ioc_wd_rx probeRx[32];
ProbePending probePending[PROBE_PENDING_LEN];
uint8_t probeSeq = 0;
uint8_t probePendingCursor = 0;

const char* acquisitionName(uint8_t mode) {
  switch (mode) {
    case ACQ_MODE_DRDY: return "DRDY";
    case ACQ_MODE_POLLING: return "POLLING";
    case ACQ_MODE_SIMULATED: return "SIM";
    case ACQ_MODE_IDLE: return "IDLE";
    default: return "UNKNOWN";
  }
}

const char* windowName(uint8_t window) {
  switch (window) {
    case 0: return "RECT";
    case 1: return "HANN";
    case 2: return "HAMMING";
    case 3: return "FLATTOP";
    case 4: return "BLACKMAN";
    default: return "UNKNOWN";
  }
}

const char* linkStateName(uint8_t state) {
  switch (state) {
    case IOC_WD_LINK_DOWN: return "DOWN";
    case IOC_WD_LINK_OFFERING: return "OFFERING";
    case IOC_WD_LINK_HANDSHAKE: return "HANDSHAKE";
    case IOC_WD_LINK_SECURE: return "SECURE";
    case IOC_WD_LINK_NO_KEY: return "NO_KEY";
    default: return "UNKNOWN";
  }
}

const char* ackStateName(uint8_t status) {
  switch (status) {
    case IOC_WD_ACK_ACCEPTED: return "ACCEPTED";
    case IOC_WD_ACK_APPLIED: return "APPLIED";
    case IOC_WD_ACK_REJECTED: return "REJECTED";
    case IOC_WD_ACK_NO_LINK: return "REJECTED";
    default: return "REJECTED";
  }
}

const char* ackReason(const ioc_wd_ack& ack) {
  if (ack.status == IOC_WD_ACK_NO_LINK) return "NO_DATA_LINK";
  if (ack.status == IOC_WD_ACK_UNKNOWN) return "UNKNOWN_TARGET_OR_OPCODE";
  if (ack.status == IOC_WD_ACK_REJECTED && ack.detail == ACK_DETAIL_TIMEOUT) return "SENSOR_TIMEOUT";
  if (ack.status == IOC_WD_ACK_REJECTED && ack.detail == ACK_DETAIL_CONFIG_UNKNOWN) return "CONFIG_UNKNOWN";
  if (ack.status == IOC_WD_ACK_REJECTED) return "SENSOR_REJECTED";
  return "";
}

float finiteOrZero(float value) {
  return std::isfinite(value) ? value : 0.0f;
}

void probePrintTelemetry(uint8_t node, uint8_t child, const ioc_wd_telemetry& t) {
  Serial.printf(
    "TEL NODE=%u CHILD=%u SEQ=%u GW_TS_MS=%lu MODE=%s ACQ=%s WIN=%u AXIS=%s FFT_VALID=%s "
    "RATE_REQ_HZ=%.2f RATE_EFF_HZ=%.2f RMS=%.5f RMS_UNIT=m/s2 KURT=%.5f CREST=%.4f "
    "PEAK_HZ=%.3f PEAK_AMP=%.6f ENT=%.4f PPV_MM_S=%.4f STA_LTA=%s CLIP=%s "
    "BATT_PCT=%u BATT_MV=%u DTC=0x%04X DTC_COUNT=%u\n",
    static_cast<unsigned>(node), static_cast<unsigned>(child), static_cast<unsigned>(t.seq),
    static_cast<unsigned long>(millis()), modeName(t.mode), acquisitionName(t.acquisition),
    static_cast<unsigned>(t.window_size), (t.axis_mask & AXIS_MASK_VECTOR) ? "VECTOR" : "Z",
    (t.flags & IOC_WD_TEL_FLAG_FFT_VALID) ? "YES" : "NO",
    static_cast<double>(finiteOrZero(t.rate_req_hz)), static_cast<double>(finiteOrZero(t.rate_eff_hz)),
    static_cast<double>(finiteOrZero(t.rms)), static_cast<double>(finiteOrZero(t.kurtosis)),
    static_cast<double>(finiteOrZero(t.crest)), static_cast<double>(finiteOrZero(t.peak_hz)),
    static_cast<double>(finiteOrZero(t.peak_amp)), static_cast<double>(finiteOrZero(t.entropy)),
    static_cast<double>(finiteOrZero(t.ppv_mm_s)),
    (t.flags & IOC_WD_TEL_FLAG_STALTA) ? "YES" : "NO",
    (t.flags & IOC_WD_TEL_FLAG_CLIPPING) ? "YES" : "NO",
    static_cast<unsigned>(t.battery_pct), static_cast<unsigned>(t.battery_mv),
    static_cast<unsigned>(t.dtc_code), static_cast<unsigned>(t.dtc_count));
}

void probePrintSpectrum(uint8_t node, uint8_t child, const ioc_wd_spectrum_header& hdr,
                        const uint8_t* binsLe) {
  // A linha é montada inteira e escrita de uma vez: a Probe volta mais cedo a
  // esvaziar o buffer de recepção CAN enquanto o Node envia o bloco seguinte.
  static char line[200 + IOC_WD_SPECTRUM_CHUNK_BINS * 4 + 2];
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  const unsigned chunks = (hdr.total_bins + IOC_WD_SPECTRUM_CHUNK_BINS - 1u) / IOC_WD_SPECTRUM_CHUNK_BINS;
  const unsigned index = hdr.offset / IOC_WD_SPECTRUM_CHUNK_BINS;
  int n = snprintf(line, 200,
                   "FRAG NODE=%u CHILD=%u TYPE=FFT TRANSFER=%u INDEX=%u COUNT=%u FORMAT=U16_SCALED "
                   "SCALE=%.6f RATE_HZ=%.2f FFT_SIZE=%u WINDOW=%s DATA=",
                   static_cast<unsigned>(node), static_cast<unsigned>(child),
                   static_cast<unsigned>(hdr.spectrum_id), index, chunks,
                   static_cast<double>(finiteOrZero(hdr.scale)),
                   static_cast<double>(finiteOrZero(hdr.sample_rate_hz)),
                   static_cast<unsigned>(hdr.fft_size), windowName(hdr.window_type));
  if (n < 0) return;
  if (n > 199) n = 199;
  uint8_t count = hdr.count;
  if (count > IOC_WD_SPECTRUM_CHUNK_BINS) count = IOC_WD_SPECTRUM_CHUNK_BINS;
  for (unsigned i = 0; i < static_cast<unsigned>(count) * 2u; ++i) {
    line[n++] = HEX_DIGITS[binsLe[i] >> 4];
    line[n++] = HEX_DIGITS[binsLe[i] & 0x0Fu];
  }
  line[n] = '\0';
  Serial.println(line);
}

void probePrintAck(uint8_t node, uint8_t child, const ioc_wd_ack& ack) {
  for (uint8_t i = 0; i < PROBE_PENDING_LEN; ++i) {
    ProbePending& pending = probePending[i];
    if (!pending.used || pending.seq != ack.seq || pending.node != node || pending.child != child) continue;
    const bool final = ack.status != IOC_WD_ACK_ACCEPTED;
    // Um SET intermediário só interessa ao operador quando falha.
    if (pending.report || (final && ack.status != IOC_WD_ACK_APPLIED)) {
      const char* reason = ackReason(ack);
      Serial.printf("ACK COMMAND=%s STATE=%s TX=%s NODE=%u CHILD=%u DETAIL=%u%s%s\n",
                    pending.action, ackStateName(ack.status), pending.tx,
                    static_cast<unsigned>(node), static_cast<unsigned>(child),
                    static_cast<unsigned>(ack.detail), reason[0] ? " REASON=" : "", reason);
    }
    // Um SET fica concluído ao ser aceito pelo Node; os demais aguardam o resultado final.
    if (final || !pending.report) pending.used = false;
    return;
  }
}

void probeHandleTransfer(uint8_t node, const ioc_wd_rx_slot& slot) {
  const uint8_t child = slot.child;
  switch (slot.kind) {
    case IOC_WD_KIND_TELEMETRY: {
      ioc_wd_telemetry t;
      if (ioc_wd_telemetry_decode(slot.data, slot.total_len, &t)) probePrintTelemetry(node, child, t);
      break;
    }
    case IOC_WD_KIND_SPECTRUM: {
      ioc_wd_spectrum_header hdr;
      const uint8_t* bins = nullptr;
      if (ioc_wd_spectrum_decode(slot.data, slot.total_len, &hdr, &bins)) {
        probePrintSpectrum(node, child, hdr, bins);
      }
      break;
    }
    case IOC_WD_KIND_CMD_ACK: {
      ioc_wd_ack ack;
      if (ioc_wd_ack_decode(slot.data, slot.total_len, &ack)) probePrintAck(node, child, ack);
      break;
    }
    case IOC_WD_KIND_DTC: {
      ioc_wd_dtc dtc;
      if (ioc_wd_dtc_decode(slot.data, slot.total_len, &dtc)) {
        Serial.printf("DTC NODE=%u CHILD=%u CODE=0x%04X SYMPTOM=0x%02X SEVERITY=%u TS_MS=%lu\n",
                      static_cast<unsigned>(node), static_cast<unsigned>(child),
                      static_cast<unsigned>(dtc.code), static_cast<unsigned>(dtc.symptom),
                      static_cast<unsigned>(dtc.severity), static_cast<unsigned long>(dtc.timestamp_ms));
      }
      break;
    }
    case IOC_WD_KIND_LINK: {
      ioc_wd_link link;
      if (ioc_wd_link_decode(slot.data, slot.total_len, &link)) {
        Serial.printf("[GW] WIRELESS_LINK node=%u child=%u state=%s rssi=%d age_s=%u rx=%u "
                      "auth_fail=%u replay=%u lost=%u stream=%s period_ms=%u\n",
                      static_cast<unsigned>(node), static_cast<unsigned>(child),
                      linkStateName(link.state), static_cast<int>(link.wifi_rssi),
                      static_cast<unsigned>(link.session_age_s), static_cast<unsigned>(link.rx_datagrams),
                      static_cast<unsigned>(link.auth_failures), static_cast<unsigned>(link.replay_drops),
                      static_cast<unsigned>(link.lost_datagrams), link.stream_enabled ? "ON" : "OFF",
                      static_cast<unsigned>(link.stream_period_ms));
      }
      break;
    }
    case IOC_WD_KIND_CONFIG: {
      ioc_wd_config cfg;
      if (ioc_wd_config_decode(slot.data, slot.total_len, &cfg)) {
        Serial.printf("[GW] WIRELESS_CONFIG node=%u child=%u status=%s mode=%s rate_req_hz=%.2f "
                      "rate_eff_hz=%.2f window=%s window_size=%u stalta=%.3f gain=%.4f\n",
                      static_cast<unsigned>(node), static_cast<unsigned>(child),
                      cfg.status == 1 ? "APPLIED" : (cfg.status == 2 ? "REJECTED" : "CURRENT"),
                      modeName(cfg.mode), static_cast<double>(finiteOrZero(cfg.rate_req_hz)),
                      static_cast<double>(finiteOrZero(cfg.rate_eff_hz)), windowName(cfg.window_type),
                      static_cast<unsigned>(cfg.window_size), static_cast<double>(finiteOrZero(cfg.stalta)),
                      static_cast<double>(finiteOrZero(cfg.gain)));
      }
      break;
    }
    default:
      break;
  }
}

uint8_t probeSendCommand(uint8_t node, uint8_t child, uint8_t opcode, const uint8_t arg[4],
                         const char* tx, const char* action, bool report) {
  ioc_wd_command cmd;
  cmd.seq = ++probeSeq;
  cmd.node_id = node;
  cmd.child_id = child;
  cmd.opcode = opcode;
  memcpy(cmd.arg, arg, 4);

  ProbePending& pending = probePending[probePendingCursor];
  probePendingCursor = static_cast<uint8_t>((probePendingCursor + 1u) % PROBE_PENDING_LEN);
  pending.used = true;
  pending.seq = cmd.seq;
  pending.node = node;
  pending.child = child;
  pending.report = report;
  strncpy(pending.tx, tx, sizeof(pending.tx) - 1);
  pending.tx[sizeof(pending.tx) - 1] = '\0';
  strncpy(pending.action, action, sizeof(pending.action) - 1);
  pending.action[sizeof(pending.action) - 1] = '\0';

  CANMessage msg;
  msg.id = CAN_ID_WIRELESS_DATA_CMD;
  msg.len = IOC_WD_FRAME_LEN;
  ioc_wd_command_encode(&cmd, msg.data);
  can.tryToSend(msg);
  return cmd.seq;
}

// Devolve o valor de KEY= dentro da linha, ou nullptr.
const char* findValue(char* tokens[], uint8_t count, const char* key) {
  const size_t keyLen = strlen(key);
  for (uint8_t i = 0; i < count; ++i) {
    if (strncasecmp(tokens[i], key, keyLen) == 0 && tokens[i][keyLen] == '=') {
      return tokens[i] + keyLen + 1;
    }
  }
  return nullptr;
}

int lookupName(const char* value, const char* const names[], uint8_t count) {
  if (!value) return -1;
  for (uint8_t i = 0; i < count; ++i) {
    if (strcasecmp(value, names[i]) == 0) return i;
  }
  return -1;
}

void probeReject(const char* action, const char* tx, const char* reason) {
  Serial.printf("ACK COMMAND=%s STATE=REJECTED TX=%s REASON=%s\n", action, tx, reason);
}

void probeHandleCommandLine(char* tokens[], uint8_t count) {
  static const char* const MODES[] = {"IDLE", "ROTATING", "STRUCTURAL", "SEISMIC"};
  static const char* const WINDOWS[] = {"RECT", "HANN", "HAMMING", "FLATTOP", "BLACKMAN"};

  const char* target = findValue(tokens, count, "TARGET");
  const char* action = findValue(tokens, count, "ACTION");
  const char* tx = findValue(tokens, count, "TX");
  if (!tx) tx = "-";
  if (!action) action = "UNKNOWN";

  unsigned node = 0, child = 0;
  if (!target || sscanf(target, "%u.%u", &node, &child) != 2 || node == 0 || node >= 32 ||
      child == 0 || child > IOC_WD_MAX_CHILD) {
    probeReject(action, tx, "INVALID_TARGET");
    return;
  }

  uint8_t arg[4] = {0, 0, 0, 0};
  const uint8_t n = static_cast<uint8_t>(node);
  const uint8_t c = static_cast<uint8_t>(child);

  if (strcasecmp(action, "STATUS") == 0 || strcasecmp(action, "WIFI_STATUS") == 0) {
    probeSendCommand(n, c, IOC_WD_OP_STATUS, arg, tx, action, true);
    return;
  }

  if (strcasecmp(action, "TELEMETRY") == 0 || strcasecmp(action, "TELEMETRY_PERIOD") == 0 ||
      strcasecmp(action, "TELEMETRY_RATE") == 0) {
    const char* state = findValue(tokens, count, "STATE");
    const char* periodText = findValue(tokens, count, "PERIOD_MS");
    const char* rateText = findValue(tokens, count, "RATE_HZ");
    uint16_t period = 0;
    arg[0] = 1;
    if (periodText) period = static_cast<uint16_t>(strtoul(periodText, nullptr, 10));
    if (rateText) {
      const float rate = strtof(rateText, nullptr);
      if (rate > 0.0f) period = static_cast<uint16_t>(1000.0f / rate);
    }
    if (state) {
      if (strcasecmp(state, "OFF") == 0) arg[0] = 0;
      else if (strcasecmp(state, "ONCE") == 0) arg[0] = 2;
      else if (strcasecmp(state, "FAST") == 0) period = 250;
      else if (strcasecmp(state, "SLOW") == 0) period = 1000;
    }
    ioc_wd_put_arg_u16(&arg[1], period);
    probeSendCommand(n, c, IOC_WD_OP_STREAM, arg, tx, action, true);
    return;
  }

  if (strcasecmp(action, "FFT") == 0) {
    const char* binsText = findValue(tokens, count, "BINS");
    ioc_wd_put_arg_u16(&arg[0], binsText ? static_cast<uint16_t>(strtoul(binsText, nullptr, 10)) : 64);
    probeSendCommand(n, c, IOC_WD_OP_FFT, arg, tx, action, true);
    return;
  }

  if (strcasecmp(action, "DTC_CLEAR") == 0) {
    probeSendCommand(n, c, IOC_WD_OP_DTC_CLEAR, arg, tx, action, true);
    return;
  }

  if (strcasecmp(action, "CONFIG") == 0) {
    const char* modeText = findValue(tokens, count, "MODE");
    const char* rateText = findValue(tokens, count, "RATE_HZ");
    if (!rateText) rateText = findValue(tokens, count, "RATE");
    const char* windowText = findValue(tokens, count, "WINDOW");
    const char* sizeText = findValue(tokens, count, "WINDOW_SIZE");
    const char* staltaText = findValue(tokens, count, "STALTA");
    const char* gainText = findValue(tokens, count, "GAIN");

    const int mode = lookupName(modeText, MODES, 4);
    const int window = lookupName(windowText, WINDOWS, 5);
    if ((modeText && mode < 0) || (windowText && window < 0)) {
      probeReject(action, tx, "INVALID_FIELD");
      return;
    }
    if (mode >= 0) {
      memset(arg, 0, 4);
      arg[0] = static_cast<uint8_t>(mode);
      probeSendCommand(n, c, IOC_WD_OP_SET_MODE, arg, tx, action, false);
    }
    if (rateText) {
      ioc_wd_put_arg_f32(arg, strtof(rateText, nullptr));
      probeSendCommand(n, c, IOC_WD_OP_SET_RATE, arg, tx, action, false);
    }
    if (window >= 0 || sizeText) {
      memset(arg, 0, 4);
      arg[0] = window >= 0 ? static_cast<uint8_t>(window) : 0xFF;
      ioc_wd_put_arg_u16(&arg[1], sizeText ? static_cast<uint16_t>(strtoul(sizeText, nullptr, 10)) : 0);
      probeSendCommand(n, c, IOC_WD_OP_SET_WINDOW, arg, tx, action, false);
    }
    if (staltaText) {
      ioc_wd_put_arg_f32(arg, strtof(staltaText, nullptr));
      probeSendCommand(n, c, IOC_WD_OP_SET_STALTA, arg, tx, action, false);
    }
    if (gainText) {
      ioc_wd_put_arg_f32(arg, strtof(gainText, nullptr));
      probeSendCommand(n, c, IOC_WD_OP_SET_GAIN, arg, tx, action, false);
    }
    memset(arg, 0, 4);
    probeSendCommand(n, c, IOC_WD_OP_APPLY, arg, tx, action, true);
    return;
  }

  // ACQ, WIFI e DTC_LIST existem apenas no console USB do sensor.
  probeReject(action, tx, "UNSUPPORTED_VIA_NODE");
}

} // namespace

/* ===================================================================== */
/* Interface pública                                                     */
/* ===================================================================== */

bool wirelessLinkHasKey() {
  return haveKey;
}

void wirelessLinkInit() {
  if (NODE_ID == 0) {
    for (uint8_t i = 0; i < 32; ++i) ioc_wd_rx_reset(&probeRx[i]);
    Serial.println("[GW] Plano de dados wireless: CMD TARGET=NN.CC ACTION=... TX=...");
    return;
  }

  haveKey = ioc_link_hex_to_key(IOT_LINK_MASTER_KEY_HEX, masterKey) && !ioc_link_key_is_zero(masterKey);
  if (haveKey) {
    ioc_link_derive_network_key(masterKey, networkKey);
  }
  // Sem chave o Node continua descobrindo e associando sensores, mas nunca
  // abre ponto de acesso nem aceita sessão (fail-closed).
  Serial.printf("[NODE %u] [WLINK] LINK_KEY=%s\n", NODE_ID, haveKey ? "PROVISIONED" : "MISSING");
}

void wirelessLinkOnBound(uint64_t uuid, uint8_t childId) {
  if (NODE_ID == 0) return;
  Link* link = findLinkByUuid(uuid);
  if (!link) {
    for (uint8_t i = 0; i < MAX_LINKS; ++i) {
      if (!links[i].used) {
        link = &links[i];
        break;
      }
    }
  }
  if (!link) return;

  *link = Link{};
  link->used = true;
  link->uuid = uuid;
  link->childId = childId;
  if (haveKey) {
    ioc_link_derive_device_key(masterKey, uuid, link->devKey);
    uint32_t seed;
    fillRandom(reinterpret_cast<uint8_t*>(&seed), sizeof(seed));
    link->offerNonce = seed;
    startAccessPoint();
  }
  publishLink(*link, true);
}

void wirelessLinkOnUnbound(uint64_t uuid, uint8_t childId) {
  (void)childId;
  if (NODE_ID == 0) return;
  Link* link = findLinkByUuid(uuid);
  if (!link) return;

  if (link->secure) sendSimpleCommand(*link, CMD_RELEASE);
  const uint8_t child = link->childId;
  purgeQueuedTransfers(child);
  ioc_secure_zero(link->devKey, sizeof(link->devKey));
  ioc_secure_zero(link->sessionKey, sizeof(link->sessionKey));
  *link = Link{};

  ioc_wd_link report;
  memset(&report, 0, sizeof(report));
  report.state = IOC_WD_LINK_DOWN;
  uint8_t raw[IOC_WD_LINK_LEN];
  ioc_wd_link_encode(&report, raw);
  queueTransfer(child, IOC_WD_KIND_LINK, raw, sizeof(raw));

  if (usedLinkCount() == 0) {
    stopOfferAdvertising();
    // O CMD_RELEASE acabou de ser entregue à pilha de rede; o ponto de
    // acesso é desligado um pouco depois, em wirelessLinkPoll().
    apStopPending = true;
    apStopAtMs = millis() + AP_STOP_DELAY_MS;
  }
}

void wirelessLinkPoll() {
  if (NODE_ID == 0) return;
  receiveDatagrams();
  maintainLinks();
  updateOfferAdvertising();
  drainTxQueue();
  if (apStopPending && static_cast<int32_t>(millis() - apStopAtMs) >= 0) {
    if (usedLinkCount() == 0) stopAccessPoint();
    apStopPending = false;
  }
}

bool wirelessLinkHandleCanMessage(const CANMessage& rx) {
  if (rx.id == CAN_ID_WIRELESS_DATA_CMD) {
    if (rx.len != IOC_WD_FRAME_LEN || NODE_ID == 0) return true;
    ioc_wd_command cmd;
    ioc_wd_command_decode(rx.data, &cmd);
    if (cmd.node_id == NODE_ID) handleDataCommand(cmd);
    return true;
  }

  if (rx.id > CAN_ID_WIRELESS_DATA_BASE && rx.id <= CAN_ID_WIRELESS_DATA_LAST) {
    if (rx.len != IOC_WD_FRAME_LEN || NODE_ID != 0) return true;
    const uint8_t node = static_cast<uint8_t>(rx.id - CAN_ID_WIRELESS_DATA_BASE);
    const ioc_wd_rx_slot* done = nullptr;
    if (ioc_wd_rx_push(&probeRx[node], rx.data, millis(), &done) == IOC_WD_RX_COMPLETE && done) {
      probeHandleTransfer(node, *done);
    }
    return true;
  }

  return false;
}

bool wirelessLinkHandleSerialCommand(const char* command) {
  if (NODE_ID != 0 || !command) return false;
  if (strncasecmp(command, "CMD ", 4) != 0) return false;

  static char line[192];
  strncpy(line, command, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';

  char* tokens[16];
  uint8_t count = 0;
  char* save = nullptr;
  for (char* token = strtok_r(line + 4, " \t", &save); token && count < 16;
       token = strtok_r(nullptr, " \t", &save)) {
    tokens[count++] = token;
  }
  probeHandleCommandLine(tokens, count);
  return true;
}
