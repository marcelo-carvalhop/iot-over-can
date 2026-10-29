#include "wireless_link.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <NimBLEDevice.h>
#include <esp_wifi.h>
#include <cmath>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if __has_include(<esp_random.h>)
#include <esp_random.h>
#else
#include <esp_system.h>
#endif

#ifdef ESP_PLATFORM
#include <atomic>   // fila de eventos do Wi-Fi (só no hardware; ver diagnóstico)
#endif

#include "can_ids.h"
#include "net_metrics.h"
#include "node_config.h"
#include "pesquisa.h"

#include "ioc_link.h"
#include "ioc_sha256.h"
#include "ioc_wdata.h"
#include "ioc_canbits.h"
#include "ioc_panel_cache.h"
#include "ioc_panel_proto.h"

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
constexpr uint16_t STREAM_PERIOD_DEFAULT_MS = IOT_STREAM_PERIOD_MS;
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
constexpr uint32_t AP_RETRY_MS = 5000;          // nova tentativa quando o ponto de acesso não sobe
constexpr uint32_t HELLO_GUARD_MS = 1000;       // um handshake recente não é substituído por outro HELLO
constexpr uint16_t ACK_DETAIL_TIMEOUT = 0xFFFF;
constexpr uint16_t ACK_DETAIL_CONFIG_UNKNOWN = 0xFFFE;
constexpr uint16_t ACK_DETAIL_PROFILE_MISMATCH = 0xFFFD; // comando de outro tipo de sensor
constexpr uint16_t ACK_DETAIL_BUSY = 0xFFFC;             // eco anterior ainda sem resposta
constexpr uint32_t ECHO_TIMEOUT_US = 2000000;            // eco pelo sensor sem PONG
constexpr uint8_t  MAX_RX_DATAGRAMS_PER_POLL = 4;
constexpr uint8_t  PROBE_PENDING_LEN = 12;
// Painel em papel eletrônico (perfil 0x04).
constexpr uint32_t TAP_FLUSH_MS = 200;            // lote ao vivo de quadros CAN para o painel
constexpr uint32_t PANEL_SLEEP_MARGIN_MS = 30000; // folga sobre o sono declarado
constexpr uint32_t PANEL_OFF_LEASE_MS = 12UL * 3600UL * 1000UL; // desligado pelo operador

/* ===================================================================== */
/* Node funcional                                                        */
/* ===================================================================== */

struct PendingHandshake {
  bool active = false;
  uint8_t profile = 0;        // perfil declarado no HELLO autenticado
  uint8_t sensorNonce[IOC_LINK_NONCE_LEN] = {0};
  uint8_t nodeNonce[IOC_LINK_NONCE_LEN] = {0};
  uint8_t sessionKey[IOC_LINK_KEY_LEN] = {0};
  IPAddress peerIp;
  uint16_t peerPort = 0;
  uint32_t startedMs = 0;
  uint32_t cpuUs = 0;         // pesquisa: CPU da criptografia desta autenticação
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
  // Perfil do sensor (0x01 vibração, 0x02 acústico), conhecido pelo HELLO da
  // sessão em vigor; 0 enquanto não houve sessão.
  uint8_t profile = 0;
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
  // Tempo de ida e volta Node <-> sensor, medido pelo par PING/PONG.
  bool pingPending = false;
  uint32_t pingCounter = 0;
  uint32_t pingSentUs = 0;
  uint16_t rtt100us = IOC_WD_RTT_UNKNOWN;
  uint16_t rttMax100us = IOC_WD_RTT_UNKNOWN;
  uint8_t sessionSensorNonce[IOC_LINK_NONCE_LEN] = {0}; // nonce do HELLO que originou a sessão

  // Pesquisa (domínio de relógio do Node), na janela entre dois relatórios:
  // custo da criptografia por datagrama e da ponte Wi-Fi -> CAN.
  uint32_t cryptoRxSumUs = 0;
  uint16_t cryptoRxN = 0;
  uint32_t cryptoTxSumUs = 0;
  uint16_t cryptoTxN = 0;
  uint32_t cryptoMaxUs = 0;
  uint32_t handshakeUs = IOC_WD_RTT_UNKNOWN;
  uint64_t bridgeSumUs = 0;
  uint16_t bridgeN = 0;
  uint32_t bridgeMaxUs = 0;
  // Eco pelo sensor (IOC_WD_OP_ECHO, caminho 1): PING próprio, sem misturar
  // com o PING periódico de vivacidade.
  bool echoPending = false;
  uint8_t echoSeq = 0;
  uint32_t echoCounter = 0;
  uint64_t echoSentUs = 0;

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
  // Numeração das amostras encaminhadas ao CAN. O Node encaminha uma amostra
  // por período e descarta as demais; a sequência do sensor teria lacunas
  // que não são perdas. A perda no Wi-Fi é contada em lostDatagrams.
  uint16_t forwardSeq = 0;
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
  bool stagedMismatch = false;   // a edição trouxe um modo de outro perfil: o APPLY é recusado
  bool applyPending = false;
  uint8_t applySeq = 0;
  uint32_t applySentMs = 0;
  bool clearPending = false;
  uint8_t clearSeq = 0;
  uint32_t clearSentMs = 0;
  // Comando acústico (reinício da medição, assinatura de referência) em andamento.
  bool acousticPending = false;
  uint8_t acousticSeq = 0;
  uint32_t acousticCounter = 0;
  uint32_t acousticSentMs = 0;

  // Painel: tap ao vivo, retrato pedido pelo HELLO do painel e sono declarado.
  bool panelSubscribed = false;   // HELLO do painel recebido nesta sessão
  bool panelEverSubscribed = false;
  bool panelAsleep = false;       // dorme por intervalo declarado: presente sem sessão
  uint32_t panelPresentUntilMs = 0;
  uint16_t panelSleepS = 0;
  uint16_t tapSeq = 0;
  size_t tapUsed = 0;
  uint32_t tapStartMs = 0;
  uint32_t tapSkippedBits = 0;
  uint8_t tapBuf[IOC_PANEL_DATAGRAM_MAX] = {0};
};

struct TxTransfer {
  uint8_t child = 0;
  uint8_t kind = 0;
  uint8_t transfer = 0;
  uint8_t len = 0;
  uint8_t next = 0;
  uint64_t originUs = 0;  // recepção do datagrama que originou a transferência (0 = local)
  uint8_t data[IOC_WD_MAX_PAYLOAD] = {0};
};

// Instante de recepção do datagrama em tratamento (domínio do Node); 0 fora dele.
uint64_t rxDatagramUs = 0;
// Duração do último selo HMAC feito por sendEnvelope().
uint32_t lastSealUs = 0;

uint32_t elapsedUs(uint64_t since) {
  const uint64_t d = pesquisaMonoUs() - since;
  return d > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(d);
}

uint16_t clampU16(uint32_t v) { return v >= IOC_WD_RTT_UNKNOWN ? static_cast<uint16_t>(IOC_WD_RTT_UNKNOWN - 1u) : static_cast<uint16_t>(v); }

Link links[MAX_LINKS];
uint8_t masterKey[IOC_LINK_KEY_LEN] = {0};
uint8_t networkKey[IOC_LINK_KEY_LEN] = {0};
bool haveKey = false;
bool apRunning = false;
bool apStopPending = false;
uint32_t apStopAtMs = 0;
// Ensaios: ponto de acesso mantido desligado até este instante.
bool apOutageActive = false;
uint32_t apOutageStartMs = 0;
uint32_t apOutageDurationMs = 0;
bool radioOff = false;
uint32_t lastApAttemptMs = 0;
uint32_t txNextTransferAtMs = 0;
bool offerAdvertising = false;
uint8_t offerCursor = 0;
uint32_t lastOfferRotateMs = 0;
WiFiUDP udp;
uint8_t rxDatagram[MAX_DATAGRAM];
uint8_t txDatagram[IOC_PANEL_DATAGRAM_MAX + IOC_ENV_OVERHEAD];
ioc_pcache panelCache;
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
  if (link.profile == IOC_PANEL_PROFILE && link.panelAsleep) return IOC_WD_LINK_SLEEPING;
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
  slot.originUs = rxDatagramUs;
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
      if (!canSend(msg)) return;
      ++slot.next;
    }
    // Ponte Wi-Fi -> CAN: do datagrama recebido ao último segmento entregue ao controlador.
    if (slot.originUs && (slot.kind == IOC_WD_KIND_TELEMETRY || slot.kind == IOC_WD_KIND_ACOUSTIC)) {
      Link* origin = findLinkByChild(slot.child);
      if (origin) {
        const uint32_t us = elapsedUs(slot.originUs);
        origin->bridgeSumUs += us;
        if (origin->bridgeN < 0xFFFF) ++origin->bridgeN;
        if (us > origin->bridgeMaxUs) origin->bridgeMaxUs = us;
      }
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

// Medidas de pesquisa da janela, logo após o relatório LINK; só com sessão
// ou com alguma medida (um vínculo ocioso não gera tráfego extra).
void publishLinkStats(Link& link) {
  const bool any = link.cryptoRxN || link.cryptoTxN || link.bridgeN || link.handshakeUs != IOC_WD_RTT_UNKNOWN;
  if (!link.secure && !any) return;
  ioc_wd_linkstats st;
  st.crypto_rx_us = link.cryptoRxN ? clampU16(link.cryptoRxSumUs / link.cryptoRxN) : IOC_WD_RTT_UNKNOWN;
  st.crypto_tx_us = link.cryptoTxN ? clampU16(link.cryptoTxSumUs / link.cryptoTxN) : IOC_WD_RTT_UNKNOWN;
  st.crypto_max_us = (link.cryptoRxN || link.cryptoTxN) ? clampU16(link.cryptoMaxUs) : IOC_WD_RTT_UNKNOWN;
  st.handshake_us = link.handshakeUs == IOC_WD_RTT_UNKNOWN ? IOC_WD_RTT_UNKNOWN : clampU16(link.handshakeUs);
  st.bridge_avg_100us = link.bridgeN ? clampU16(static_cast<uint32_t>(link.bridgeSumUs / link.bridgeN / 100u))
                                     : IOC_WD_RTT_UNKNOWN;
  st.bridge_max_100us = link.bridgeN ? clampU16(link.bridgeMaxUs / 100u) : IOC_WD_RTT_UNKNOWN;
  link.cryptoRxSumUs = link.cryptoTxSumUs = link.cryptoMaxUs = 0;  // nova janela
  link.cryptoRxN = link.cryptoTxN = 0;
  link.bridgeSumUs = 0;
  link.bridgeN = 0;
  link.bridgeMaxUs = 0;
  uint8_t raw[IOC_WD_LINKSTATS_LEN];
  ioc_wd_linkstats_encode(&st, raw);
  const uint64_t keep = rxDatagramUs;
  rxDatagramUs = 0;  // relatórios não são ponte de dados do sensor
  queueTransfer(link.childId, IOC_WD_KIND_LINKSTATS, raw, sizeof(raw));
  rxDatagramUs = keep;
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
  report.rtt_100us = link.secure ? link.rtt100us : IOC_WD_RTT_UNKNOWN;
  report.rtt_max_100us = link.secure ? link.rttMax100us : IOC_WD_RTT_UNKNOWN;
  uint8_t raw[IOC_WD_LINK_LEN];
  ioc_wd_link_encode(&report, raw);
  queueTransfer(link.childId, IOC_WD_KIND_LINK, raw, sizeof(raw));
  publishLinkStats(link);
}

void publishConfig(const Link& link, uint8_t status) {
  if (!link.current.known) return;
  ioc_wd_config cfg;
  cfg.profile = link.profile;
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

/* -------------------------------------------------------- diagnóstico */

// Uma linha no monitor serial do Node para cada passo do enlace com o
// sensor: anúncio da oferta, estações no ponto de acesso, HELLO recebido e o
// resultado da conferência. Essas linhas não vão ao barramento; servem para
// localizar, em bancada, a etapa em que a associação para. Mensagens de
// recusa repetidas saem no máximo uma vez por segundo.
constexpr uint32_t DIAG_MIN_INTERVAL_MS = 1000;
uint32_t diagRejectMs = 0;
uint64_t diagOfferUuid = 0;
bool diagOfferOk = false;

void formatIp(const IPAddress& ip, char* out, size_t len) {
  snprintf(out, len, "%u.%u.%u.%u", static_cast<unsigned>(ip[0]), static_cast<unsigned>(ip[1]),
           static_cast<unsigned>(ip[2]), static_cast<unsigned>(ip[3]));
}

bool diagRejectAllowed() {
  const uint32_t now = millis();
  if (diagRejectMs != 0 && now - diagRejectMs < DIAG_MIN_INTERVAL_MS) return false;
  diagRejectMs = now ? now : 1u;
  return true;
}

void diagDatagram(const char* kind, const IPAddress& ip, uint16_t port, uint64_t uuid, const char* outcome,
                  bool reject) {
  if (reject && !diagRejectAllowed()) return;
  char addr[16];
  formatIp(ip, addr, sizeof(addr));
  Serial.printf("[NODE %u] [WLINK] %s de %s:%u uuid=0x%016llX: %s\n", NODE_ID, kind, addr,
                static_cast<unsigned>(port), static_cast<unsigned long long>(uuid), outcome);
}

#ifdef ESP_PLATFORM
// Os eventos do ponto de acesso chegam na tarefa do driver Wi-Fi. São
// guardados nesta fila (um produtor, um consumidor) e impressos pelo laço
// principal, que é quem escreve na serial.
struct StationEvent {
  uint8_t kind;      // 1 conectada, 2 desconectada, 3 endereço atribuído
  uint8_t mac[6];
  uint32_t ip;       // ordem de rede
};
constexpr uint8_t STATION_EVENTS = 8;
StationEvent stationEvents[STATION_EVENTS];
std::atomic<uint8_t> stationHead{0};
std::atomic<uint8_t> stationTail{0};
bool stationEventsRegistered = false;

void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  StationEvent e{};
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      e.kind = 1;
      memcpy(e.mac, info.wifi_ap_staconnected.mac, sizeof(e.mac));
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      e.kind = 2;
      memcpy(e.mac, info.wifi_ap_stadisconnected.mac, sizeof(e.mac));
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      e.kind = 3;
      e.ip = info.wifi_ap_staipassigned.ip.addr;
      break;
    default:
      return;
  }
  const uint8_t head = stationHead.load(std::memory_order_relaxed);
  const uint8_t next = static_cast<uint8_t>((head + 1u) % STATION_EVENTS);
  if (next == stationTail.load(std::memory_order_acquire)) return;   // fila cheia: descarta
  stationEvents[head] = e;
  stationHead.store(next, std::memory_order_release);
}

void registerStationEvents() {
  if (stationEventsRegistered) return;
  WiFi.onEvent(onWiFiEvent);
  stationEventsRegistered = true;
}

void printStationEvents() {
  uint8_t tail = stationTail.load(std::memory_order_relaxed);
  while (tail != stationHead.load(std::memory_order_acquire)) {
    const StationEvent& e = stationEvents[tail];
    if (e.kind == 3) {
      Serial.printf("[NODE %u] [WLINK] estacao recebeu ip=%u.%u.%u.%u\n", NODE_ID,
                    static_cast<unsigned>(e.ip & 0xFFu), static_cast<unsigned>((e.ip >> 8) & 0xFFu),
                    static_cast<unsigned>((e.ip >> 16) & 0xFFu), static_cast<unsigned>((e.ip >> 24) & 0xFFu));
    } else {
      Serial.printf("[NODE %u] [WLINK] estacao %s mac=%02X:%02X:%02X:%02X:%02X:%02X\n", NODE_ID,
                    e.kind == 1 ? "conectada" : "desconectada", e.mac[0], e.mac[1], e.mac[2], e.mac[3],
                    e.mac[4], e.mac[5]);
    }
    tail = static_cast<uint8_t>((tail + 1u) % STATION_EVENTS);
    stationTail.store(tail, std::memory_order_release);
  }
}
#else
void registerStationEvents() {}
void printStationEvents() {}
#endif

void startAccessPoint() {
  apStopPending = false; // um novo vínculo cancela o desligamento agendado
  if (apRunning || !haveKey || apOutageActive || radioOff) return;
  lastApAttemptMs = millis();
  char ssid[IOC_LINK_SSID_LEN];
  char psk[IOC_LINK_PSK_LEN];
  ioc_link_wifi_ssid(NODE_ID, ssid);
  ioc_link_wifi_psk(networkKey, NODE_ID, psk);
  WiFi.persistent(false); // SSID e senha derivados não são gravados na NVS
  registerStationEvents();
  WiFi.mode(WIFI_AP);
  const bool ok = WiFi.softAP(ssid, psk, ioc_link_wifi_channel(NODE_ID), 0, AP_MAX_STATIONS);
  ioc_secure_zero(psk, sizeof(psk));
  if (!ok) {
    Serial.printf("[NODE %u] [WLINK] AP=ERROR\n", NODE_ID);
    return;
  }
  const bool listening = udp.begin(IOC_LINK_UDP_PORT);
  apRunning = true;
  char addr[16];
  formatIp(WiFi.softAPIP(), addr, sizeof(addr));
  Serial.printf("[NODE %u] [WLINK] AP=ON ssid=%s channel=%u ip=%s udp=%u%s\n", NODE_ID, ssid,
                static_cast<unsigned>(ioc_link_wifi_channel(NODE_ID)), addr,
                static_cast<unsigned>(IOC_LINK_UDP_PORT), listening ? "" : " (falha ao abrir a porta)");
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
  diagOfferUuid = 0;
  Serial.printf("[NODE %u] [WLINK] OFERTA encerrada\n", NODE_ID);
}

// Anuncia a oferta de um sensor que ainda não tem sessão. Havendo mais de um,
// alterna entre eles.
void updateOfferAdvertising() {
  if (!haveKey) return;
  if (apOutageActive || radioOff) {
    stopOfferAdvertising(); // sem ponto de acesso não há para onde chamar o sensor
    return;
  }
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
  if (target->uuid != diagOfferUuid || offerAdvertising != diagOfferOk) {
    diagOfferUuid = target->uuid;
    diagOfferOk = offerAdvertising;
    Serial.printf("[NODE %u] [WLINK] OFERTA uuid=0x%016llX canal=%u: %s\n", NODE_ID,
                  static_cast<unsigned long long>(target->uuid), static_cast<unsigned>(offer.channel),
                  offerAdvertising ? "anunciando por BLE" : "falha ao iniciar o anuncio BLE");
  }
}

/* ----------------------------------------------------- envio ao sensor */

bool sendEnvelope(const uint8_t* key, uint8_t type, uint32_t counter, const void* payload,
                  size_t len, const IPAddress& ip, uint16_t port) {
  const uint64_t sealStart = pesquisaMonoUs();
  const size_t total = ioc_env_seal(key, type, counter, static_cast<const uint8_t*>(payload), len,
                                    txDatagram, sizeof(txDatagram));
  lastSealUs = elapsedUs(sealStart);
  if (total == 0) return false;
  if (!udp.beginPacket(ip, port)) return false;
  udp.write(txDatagram, total);
  return udp.endPacket() == 1;
}

bool sendToSensor(Link& link, const void* inner, size_t len) {
  if (!link.secure || link.txCounter == 0xFFFFFFFFu) return false;
  const bool sent = sendEnvelope(link.sessionKey, IOC_MSG_DATA_DOWN, ++link.txCounter, inner, len,
                                 link.peerIp, link.peerPort);
  link.cryptoTxSumUs += lastSealUs;
  if (link.cryptoTxN < 0xFFFF) ++link.cryptoTxN;
  if (lastSealUs > link.cryptoMaxUs) link.cryptoMaxUs = lastSealUs;
  return sent;
}

void sendPing(Link& link) {
  Payload_Heartbeat ping;
  memset(&ping, 0, sizeof(ping));
  ping.magic_header = NET_MAGIC_HEADER;
  ping.cmd_type = CMD_PING;
  ping.request_counter = link.txCounter + 1u;
  link.pingCounter = ping.request_counter;
  link.pingSentUs = micros();
  link.pingPending = sendToSensor(link, &ping, sizeof(ping));
  link.lastPingMs = millis();
}

// O sensor devolve no PONG o contador do PING. A medida inclui o tempo de
// processamento do sensor e o intervalo até este Node ler o datagrama.
void handlePong(Link& link, const uint8_t* inner, size_t len) {
  if (len < sizeof(Payload_Heartbeat)) return;
  Payload_Heartbeat pong;
  memcpy(&pong, inner, sizeof(pong));
  if (link.echoPending && pong.request_counter == link.echoCounter) {
    // Eco de pesquisa: o detalhe da confirmação leva a ida e volta Wi-Fi medida
    // aqui (domínio do Node), em 0,1 ms; a Probe mede o total no domínio dela.
    link.echoPending = false;
    publishAck(link.childId, link.echoSeq, IOC_WD_OP_ECHO, IOC_WD_ACK_APPLIED,
               clampU16(elapsedUs(link.echoSentUs) / 100u));
    return;
  }
  if (!link.pingPending || pong.request_counter != link.pingCounter) return;
  link.pingPending = false;
  const uint32_t elapsed100us = (micros() - link.pingSentUs) / 100u;
  const uint16_t rtt = elapsed100us >= IOC_WD_RTT_UNKNOWN ? static_cast<uint16_t>(IOC_WD_RTT_UNKNOWN - 1u)
                                                          : static_cast<uint16_t>(elapsed100us);
  link.rtt100us = rtt;
  if (link.rttMax100us == IOC_WD_RTT_UNKNOWN || rtt > link.rttMax100us) link.rttMax100us = rtt;
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
  link.stagedMismatch = false;
  link.pingPending = false;
  link.rtt100us = IOC_WD_RTT_UNKNOWN;
  link.rttMax100us = IOC_WD_RTT_UNKNOWN;
  link.acousticPending = false;
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

const char* acousticModeName(uint8_t mode) {
  switch (mode) {
    case ACOUSTIC_MODE_IDLE: return "IDLE";
    case ACOUSTIC_MODE_LEVEL: return "LEVEL";
    case ACOUSTIC_MODE_MACHINE: return "MACHINE";
    case ACOUSTIC_MODE_EVENTS: return "EVENTS";
    default: return "UNKNOWN";
  }
}

// O número do modo só tem significado junto com o perfil do sensor.
const char* modeNameFor(uint8_t profile, uint8_t mode) {
  return profile == PROFILE_ACOUSTIC_MEMS ? acousticModeName(mode) : modeName(mode);
}

const char* profileTag(uint8_t profile) {
  switch (profile) {
    case PROFILE_ACCELEROMETER: return "VIBRATION";
    case PROFILE_ACOUSTIC_MEMS: return "ACOUSTIC";
    case IOC_PANEL_PROFILE: return "PANEL";
    default: return "UNKNOWN";
  }
}

// Perda no Wi-Fi: a numeração do sensor é contígua dentro da sessão.
void noteSensorSequence(Link& link, uint16_t seq) {
  if (link.haveSeq) {
    const uint16_t gap = static_cast<uint16_t>(seq - link.lastSeq);
    if (gap > 1u && gap < 0x8000u) {
      const uint32_t lost = static_cast<uint32_t>(link.lostDatagrams) + (gap - 1u);
      link.lostDatagrams = lost > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(lost);
    }
  }
  link.haveSeq = true;
  link.lastSeq = seq;
}

// Uma amostra por período de encaminhamento (ou a única pedida) segue ao CAN.
bool forwardDue(const Link& link, uint32_t now) {
  return link.streamOnce || (link.streamEnabled && now - link.lastForwardMs >= link.streamPeriodMs);
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

  noteSensorSequence(link, t.seq_num);

  const uint32_t now = millis();
  if (forwardDue(link, now)) {
    ioc_wd_telemetry out;
    out.seq = link.forwardSeq++;
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

// Relatório acústico (um por segundo): níveis já integrados no sensor seguem
// como transferência de 108 bytes; o espectro anexado atende a um pedido de FFT.
void handleAcoustic(Link& link, const uint8_t* inner, size_t len) {
  Payload_AcousticStream a;
  if (len < sizeof(a)) return;
  memcpy(&a, inner, sizeof(a));
  if (len - sizeof(a) != static_cast<size_t>(a.spectrum_bins) * sizeof(float)) return; // tamanho incoerente

  noteSensorSequence(link, a.seq_num);

  const uint32_t now = millis();
  if (forwardDue(link, now)) {
    ioc_wd_acoustic out;
    memset(&out, 0, sizeof(out));
    out.seq = link.forwardSeq++;
    out.mode = a.mode;
    out.flags = a.flags;
    const float rate = std::isfinite(a.sample_rate_eff_hz) ? a.sample_rate_eff_hz : 0.0f;
    out.rate_hz = rate <= 0.0f ? 0u : (rate >= 65535.0f ? 65535u : static_cast<uint16_t>(rate + 0.5f));
    out.fft_size = a.fft_size;
    out.interval_ms = a.interval_ms;
    out.coverage_pct = a.coverage_pct;
    out.cpu_pct = a.cpu_pct;
    out.laeq_cb = a.laeq_cb;
    out.lceq_cb = a.lceq_cb;
    out.lzeq_cb = a.lzeq_cb;
    out.lzpeak_cb = a.lzpeak_cb;
    out.laf_cb = a.laf_cb;
    out.lafmax_cb = a.lafmax_cb;
    out.lafmin_cb = a.lafmin_cb;
    out.crest_x100 = a.crest_x100;
    out.kurtosis_x100 = a.kurtosis_x100;
    out.peak_hz = a.peak_hz;
    out.centroid_hz = a.centroid_hz;
    out.flatness_x10000 = a.flatness_x10000;
    out.leq_total_cb = a.leq_total_cb;
    out.l10_cb = a.l10_cb;
    out.l50_cb = a.l50_cb;
    out.l90_cb = a.l90_cb;
    out.dose_nr15_x10 = a.dose_nr15_x10;
    out.dose_nho01_x10 = a.dose_nho01_x10;
    out.elapsed_s = a.elapsed_s;
    out.tonal_cb = a.tonal_cb;
    out.f0_hz = a.f0_hz;
    out.harmonics = a.harmonics;
    out.anomaly_x100 = a.anomaly_x100;
    out.baseline_pct = a.baseline_pct;
    out.event_count = a.event_count;
    out.events_interval = a.events_interval;
    out.event_class = a.event_class;
    out.event_peak_cb = a.event_peak_cb;
    out.event_sel_cb = a.event_sel_cb;
    out.event_duration_ms = a.event_duration_ms;
    out.event_centroid_hz = a.event_centroid_hz;
    out.event_age_s = a.event_age_s;
    out.sta_lta_cb = a.sta_lta_cb;
    out.background_cb = a.background_cb;
    for (unsigned b = 0; b < IOC_WD_ACOUSTIC_BANDS; ++b) out.octave_cb[b] = a.octave_cb[b];
    out.battery_pct = a.battery_pct;
    out.battery_mv = a.battery_mv;
    out.dtc_count = a.dtc_count;
    out.dtc_code = a.dtc_active_code;
    uint8_t raw[IOC_WD_ACOUSTIC_LEN];
    ioc_wd_acoustic_encode(&out, raw);
    if (queueTransfer(link.childId, IOC_WD_KIND_ACOUSTIC, raw, sizeof(raw))) {
      link.lastForwardMs = now;
      link.streamOnce = false;
    }
  }

  if (link.fftBinsWanted > 0) {
    if (a.spectrum_bins > 0) {
      forwardSpectrum(link, inner + sizeof(a), a.spectrum_bins, a.sample_rate_eff_hz);
    } else if (a.mode == ACOUSTIC_MODE_IDLE) {
      publishAck(link.childId, link.fftSeq, IOC_WD_OP_FFT, IOC_WD_ACK_REJECTED, 0);
      link.fftBinsWanted = 0;
    }
    // Sem espectro neste segundo (modo EVENTS sobrecarregado): espera o próximo.
  }
}

void handleAcousticAck(Link& link, const uint8_t* inner, size_t len) {
  Payload_AcousticControlAck ack;
  if (len != sizeof(ack) || !link.acousticPending) return;
  memcpy(&ack, inner, sizeof(ack));
  if (ack.request_counter != link.acousticCounter) return; // resposta a um pedido antigo
  publishAck(link.childId, link.acousticSeq, IOC_WD_OP_ACOUSTIC,
             ack.status == 1 ? IOC_WD_ACK_APPLIED : IOC_WD_ACK_REJECTED, ack.action);
  link.acousticPending = false;
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

/* ------------------------------------------------ painel em papel eletrônico */

// O contrato entre Node e painel é o próprio barramento: o Node encaminha os
// quadros que observa (os recebidos e os que ele mesmo transmite, que o
// MCP2515 não devolve), o retrato do estado e a caixa postal acumulada
// enquanto o painel dormia. Não interpreta nada além do necessário para
// escolher o que guardar (ioc_panel_cache).

bool isPanel(const Link& link) { return link.profile == IOC_PANEL_PROFILE; }

void updatePanelMailbox() {
  bool open = false;
  for (uint8_t i = 0; i < MAX_LINKS; ++i) {
    const Link& link = links[i];
    if (link.used && isPanel(link) && link.panelEverSubscribed && !(link.secure && link.panelSubscribed)) open = true;
  }
  ioc_pcache_mailbox_open(&panelCache, open);
}

void tapReset(Link& link, uint8_t flags) {
  ioc_panel_frames_hdr hdr = {flags, link.tapSeq++, 0, 0, 0};
  link.tapUsed = ioc_panel_frames_begin(link.tapBuf, sizeof(link.tapBuf), &hdr);
  link.tapSkippedBits = 0;
  link.tapStartMs = millis();
}

void tapFlush(Link& link, bool force) {
  if (!link.secure || !link.panelSubscribed || link.tapUsed < IOC_PANEL_FRAMES_HEADER) return;
  const uint32_t now = millis();
  if (!force && now - link.tapStartMs < TAP_FLUSH_MS) return;
  const uint32_t span = now - link.tapStartMs;
  link.tapBuf[6] = static_cast<uint8_t>(span > 0xFFFFu ? 0xFFFFu : span);
  link.tapBuf[7] = static_cast<uint8_t>((span > 0xFFFFu ? 0xFFFFu : span) >> 8);
  for (uint8_t i = 0; i < 4; ++i) link.tapBuf[8 + i] = static_cast<uint8_t>(link.tapSkippedBits >> (8 * i));
  sendToSensor(link, link.tapBuf, link.tapUsed);
  tapReset(link, IOC_PANEL_FR_LIVE);
}

struct SnapshotSink {
  Link* link;
  uint8_t flags;
  size_t used;
  uint8_t buf[IOC_PANEL_DATAGRAM_MAX];
};

void snapshotBegin(SnapshotSink& sink) {
  ioc_panel_frames_hdr hdr = {sink.flags, sink.link->tapSeq++, 0, 0, 0};
  sink.used = ioc_panel_frames_begin(sink.buf, sizeof(sink.buf), &hdr);
}

void snapshotEmit(void* ctx, const ioc_panel_frame* frame) {
  SnapshotSink& sink = *static_cast<SnapshotSink*>(ctx);
  size_t next = ioc_panel_frames_add(sink.buf, sizeof(sink.buf), sink.used, frame);
  if (next == 0) {
    sendToSensor(*sink.link, sink.buf, sink.used);
    snapshotBegin(sink);
    next = ioc_panel_frames_add(sink.buf, sizeof(sink.buf), sink.used, frame);
  }
  if (next) sink.used = next;
}

void panelSendSnapshot(Link& link) {
  static SnapshotSink sink;
  sink.link = &link;
  sink.flags = IOC_PANEL_FR_SNAPSHOT;
  snapshotBegin(sink);
  ioc_pcache_snapshot(&panelCache, millis(), snapshotEmit, &sink);
  if (ioc_panel_frames_count(sink.buf) > 0) {
    sendToSensor(link, sink.buf, sink.used);
    sink.flags = IOC_PANEL_FR_MAILBOX;
    snapshotBegin(sink);
  } else {
    sink.flags = IOC_PANEL_FR_MAILBOX;
    ioc_panel_frames_set_flags(sink.buf, sink.flags);
  }
  ioc_pcache_mailbox_drain(&panelCache, millis(), snapshotEmit, &sink);
  ioc_panel_frames_set_flags(sink.buf, static_cast<uint8_t>(sink.flags | IOC_PANEL_FR_END));
  sendToSensor(link, sink.buf, sink.used);
}

void panelToCan(const Link& link, uint8_t type, const uint8_t* body, size_t len) {
  uint8_t raw[IOC_WD_MAX_PAYLOAD];
  if (len + 1 > sizeof(raw)) return;
  raw[0] = type;
  memcpy(raw + 1, body, len);
  queueTransfer(link.childId, IOC_WD_KIND_PANEL, raw, len + 1);
}

void handlePanelPayload(Link& link, uint8_t cmd, const uint8_t* body, size_t len, const uint8_t* inner, size_t innerLen) {
  const uint32_t now = millis();
  if (!isPanel(link)) return;
  if (cmd == IOC_PANEL_CMD_HELLO) {
    ioc_panel_hello hello;
    if (!ioc_panel_hello_decode(inner, innerLen, &hello)) return;
    link.panelSleepS = hello.sleep_s;
    link.panelAsleep = false;
    tapReset(link, IOC_PANEL_FR_LIVE);
    if (hello.flags & IOC_PANEL_HELLO_SNAPSHOT) panelSendSnapshot(link);
    link.panelSubscribed = true;
    link.panelEverSubscribed = true;
    tapReset(link, IOC_PANEL_FR_LIVE);
    updatePanelMailbox();
    Serial.printf("[NODE %u] [PANEL] child=%u retrato enviado sleep_s=%u\n", NODE_ID,
                  static_cast<unsigned>(link.childId), static_cast<unsigned>(hello.sleep_s));
    return;
  }
  if (cmd == IOC_PANEL_CMD_STATUS) {
    ioc_panel_status st;
    if (!ioc_panel_status_decode_body(body, len, &st)) return;
    if (st.flags & (IOC_PANEL_ST_SLEEP | IOC_PANEL_ST_POWER_OFF)) {
      link.panelAsleep = true;
      link.panelSleepS = st.sleep_s;
      link.panelPresentUntilMs = now + ((st.flags & IOC_PANEL_ST_POWER_OFF) ? PANEL_OFF_LEASE_MS
                                                                           : st.sleep_s * 1000UL + PANEL_SLEEP_MARGIN_MS);
      link.panelSubscribed = false;  // o tap para; a caixa postal abre
      updatePanelMailbox();
    }
    panelToCan(link, IOC_PANEL_CMD_STATUS, body, IOC_PANEL_STATUS_BODY);
    return;
  }
  if (cmd == IOC_PANEL_CMD_MARK) {
    ioc_panel_mark mark;
    if (!ioc_panel_mark_decode_body(body, len, &mark)) return;
    panelToCan(link, IOC_PANEL_CMD_MARK, body, IOC_PANEL_MARK_FIXED + mark.text_len);
    uint8_t ack[3 + IOC_PANEL_MARK_ACK_BODY];
    const uint8_t seq[IOC_PANEL_MARK_ACK_BODY] = {static_cast<uint8_t>(mark.seq), static_cast<uint8_t>(mark.seq >> 8)};
    const size_t n = ioc_panel_msg(IOC_PANEL_CMD_MARK_ACK, seq, sizeof(seq), ack, sizeof(ack));
    sendToSensor(link, ack, n);
  }
}

void tapFrame(const CANMessage& msg, bool own) {
  ioc_pcache_on_frame(&panelCache, static_cast<uint16_t>(msg.id), msg.data, msg.len, own ? 1 : 0, millis());
  const bool live = ioc_pcache_forward_live(static_cast<uint16_t>(msg.id), msg.data, msg.len) != 0;
  for (uint8_t i = 0; i < MAX_LINKS; ++i) {
    Link& link = links[i];
    if (!link.used || !link.secure || !link.panelSubscribed) continue;
    if (!live) {
      link.tapSkippedBits += ioc_can_frame_bits(static_cast<uint16_t>(msg.id), msg.len, msg.data);
      continue;
    }
    ioc_panel_frame frame;
    frame.age_ms = 0;
    frame.id = static_cast<uint16_t>(msg.id);
    frame.own = own ? 1 : 0;
    frame.len = msg.len > 8 ? 8 : msg.len;
    memcpy(frame.data, msg.data, frame.len);
    size_t next = ioc_panel_frames_add(link.tapBuf, sizeof(link.tapBuf), link.tapUsed, &frame);
    if (next == 0) {
      tapFlush(link, true);
      next = ioc_panel_frames_add(link.tapBuf, sizeof(link.tapBuf), link.tapUsed, &frame);
    }
    if (next) link.tapUsed = next;
  }
}

void handleSensorPayload(Link& link, const uint8_t* inner, size_t len) {
  if (len < 3) return;
  uint16_t magic;
  memcpy(&magic, inner, 2);
  if (magic != NET_MAGIC_HEADER) return;

  const uint8_t* panelBody = nullptr;
  size_t panelLen = 0;
  const uint8_t panelCmd = ioc_panel_msg_parse(inner, len, &panelBody, &panelLen);
  if (panelCmd) {
    handlePanelPayload(link, panelCmd, panelBody, panelLen, inner, len);
    return;
  }

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
      handlePong(link, inner, len);
      break;
    case CMD_ACOUSTIC_STREAM:
      handleAcoustic(link, inner, len);
      break;
    case CMD_ACOUSTIC_CONTROL_ACK:
      handleAcousticAck(link, inner, len);
      break;
    default:
      break;
  }
}

/* --------------------------------------------------------- handshake */

void handleHello(const uint8_t* datagram, size_t len, const ioc_env_view& view,
                 const IPAddress& ip, uint16_t port) {
  ioc_hello_t hello;
  if (!ioc_hello_decode(view.payload, view.payload_len, &hello)) {
    diagDatagram("HELLO", ip, port, 0, "formato invalido", true);
    return;
  }

  // Só existe chave para sensores que o operador associou a este Node. Um
  // HELLO de UUID desconhecido é ignorado sem resposta.
  Link* link = findLinkByUuid(hello.uuid);
  if (!link) {
    diagDatagram("HELLO", ip, port, hello.uuid, "sensor nao associado a este Node", true);
    return;
  }
  const uint64_t helloStart = pesquisaMonoUs();
  if (!ioc_env_verify(link->devKey, datagram, len)) {
    if (link->authFailures < 0xFFFF) ++link->authFailures;
    diagDatagram("HELLO", ip, port, hello.uuid, "chave nao confere", true);
    return;
  }
  uint32_t helloCpuUs = elapsedUs(helloStart);
  if (hello.protocol != IOC_LINK_VERSION) {
    diagDatagram("HELLO", ip, port, hello.uuid, "versao de protocolo diferente", true);
    return;
  }

  // O HELLO não carrega nada que o Node possa conferir quanto à atualidade,
  // então uma cópia capturada continua autêntica. Três regras limitam o que
  // uma repetição consegue fazer:
  //  - o HELLO que originou a sessão em vigor é ignorado;
  //  - o mesmo HELLO recebido de novo reenvia o mesmo CHALLENGE;
  //  - um handshake recém-iniciado não é substituído por outro HELLO.
  PendingHandshake& pending = link->pending;
  if (link->secure && ioc_ct_equal(hello.sensor_nonce, link->sessionSensorNonce, IOC_LINK_NONCE_LEN)) {
    if (link->replayDrops < 0xFFFF) ++link->replayDrops;
    diagDatagram("HELLO", ip, port, hello.uuid, "repeticao do HELLO da sessao em vigor, ignorado", true);
    return;
  }
  const bool sameHello = pending.active &&
                         ioc_ct_equal(hello.sensor_nonce, pending.sensorNonce, IOC_LINK_NONCE_LEN);
  if (pending.active && !sameHello && millis() - pending.startedMs < HELLO_GUARD_MS) {
    diagDatagram("HELLO", ip, port, hello.uuid, "autenticacao anterior em andamento, ignorado", true);
    return;
  }

  if (!sameHello) {
    pending.active = true;
    pending.profile = hello.profile;
    pending.startedMs = millis();
    memcpy(pending.sensorNonce, hello.sensor_nonce, IOC_LINK_NONCE_LEN);
    fillRandom(pending.nodeNonce, IOC_LINK_NONCE_LEN);
    const uint64_t deriveStart = pesquisaMonoUs();
    ioc_link_derive_session_key(link->devKey, link->uuid, NODE_ID, pending.sensorNonce,
                                pending.nodeNonce, pending.sessionKey);
    helloCpuUs += elapsedUs(deriveStart);
    pending.cpuUs = 0;
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
  const bool sent = sendEnvelope(link->devKey, IOC_MSG_CHALLENGE, 0, raw, sizeof(raw), ip, port);
  pending.cpuUs += helloCpuUs + lastSealUs;
  diagDatagram("HELLO", ip, port, hello.uuid, sent ? "conferido, CHALLENGE enviado" : "conferido, falha ao enviar o CHALLENGE",
               false);
  publishLink(*link);
}

// Promove o handshake pendente a sessão em vigor. `counter` é o contador do
// datagrama que acabou de ser autenticado com a nova chave.
void promotePending(Link& link, const IPAddress& ip, uint16_t port, uint32_t counter) {
  link.handshakeUs = link.pending.cpuUs;
  link.echoPending = false;
  memcpy(link.sessionKey, link.pending.sessionKey, IOC_LINK_KEY_LEN);
  memcpy(link.sessionSensorNonce, link.pending.sensorNonce, IOC_LINK_NONCE_LEN);
  ioc_secure_zero(link.pending.sessionKey, IOC_LINK_KEY_LEN);
  link.pending.active = false;
  link.secure = true;
  link.profile = link.pending.profile;
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
  link.acousticPending = false;
  link.fftBinsWanted = 0;
  link.pingPending = false;
  link.rtt100us = IOC_WD_RTT_UNKNOWN;
  link.rttMax100us = IOC_WD_RTT_UNKNOWN;
  // Uma sessão nova pode ser um sensor que reiniciou com outra configuração:
  // o que se sabia deixa de valer até o menu chegar.
  link.current.known = false;
  link.staged = link.current;
  link.stagedDirty = false;
  link.stagedMismatch = false;
  // O sensor envia o menu de configuração após o CONFIRM, mas um datagrama
  // UDP pode se perder; o pedido explícito cobre esse caso.
  link.panelSubscribed = false;
  link.panelAsleep = false;
  if (!isPanel(link)) sendSimpleCommand(link, CMD_GET_CONFIG_MENU);
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
    const uint64_t confirmStart = pesquisaMonoUs();
    if (!ioc_env_verify(link.pending.sessionKey, datagram, len)) continue;
    link.pending.cpuUs += elapsedUs(confirmStart);
    if (!ioc_ct_equal(confirm.node_nonce, link.pending.nodeNonce, IOC_LINK_NONCE_LEN) ||
        !ioc_ct_equal(confirm.sensor_nonce, link.pending.sensorNonce, IOC_LINK_NONCE_LEN)) {
      continue;
    }
    // O sensor provou conhecer K_dev e estar respondendo ao nonce gerado
    // agora por este Node.
    promotePending(link, ip, port, view.counter);
    return;
  }
  diagDatagram("CONFIRM", ip, port, 0, "nao confere com nenhuma autenticacao em andamento", true);
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
  const uint64_t verifyStart = pesquisaMonoUs();
  const bool authentic = ioc_env_verify(link->sessionKey, datagram, len) != 0;
  const uint32_t verifyUs = elapsedUs(verifyStart);
  if (authentic) {
    link->cryptoRxSumUs += verifyUs;
    if (link->cryptoRxN < 0xFFFF) ++link->cryptoRxN;
    if (verifyUs > link->cryptoMaxUs) link->cryptoMaxUs = verifyUs;
  }
  if (!authentic) {
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
    const IPAddress ip = udp.remoteIP();
    const uint16_t port = udp.remotePort();
    ioc_env_view view;
    if (got < static_cast<int>(IOC_ENV_OVERHEAD) || !ioc_env_parse(rxDatagram, static_cast<size_t>(got), &view)) {
      diagDatagram("datagrama", ip, port, 0, "formato desconhecido, descartado", true);
      continue;
    }
    // Medida da ponte: do instante em que o laço lê o datagrama.
    rxDatagramUs = view.type == IOC_MSG_DATA_UP ? pesquisaMonoUs() : 0;
    switch (view.type) {
      case IOC_MSG_HELLO:   handleHello(rxDatagram, got, view, ip, port); break;
      case IOC_MSG_CONFIRM: handleConfirm(rxDatagram, got, view, ip, port); break;
      case IOC_MSG_DATA_UP: handleDataUp(rxDatagram, got, view, ip, port); break;
      default: break;
    }
    rxDatagramUs = 0;
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

    if (link.panelAsleep && !link.secure && static_cast<int32_t>(now - link.panelPresentUntilMs) > 0) {
      link.panelAsleep = false;  // não voltou no prazo declarado: segue a presença normal
      publishLink(link, true);
    }
    if (link.secure) {
      if (now - link.lastRxMs > SESSION_TIMEOUT_MS) {
        Serial.printf("[NODE %u] [WLINK] child=%u sessao %s\n", NODE_ID,
                      static_cast<unsigned>(link.childId), link.panelAsleep ? "encerrada (painel dormindo)" : "expirada");
        dropSession(link);
        updatePanelMailbox();
        publishLink(link, true);
      } else if (now - link.lastPingMs >= PING_INTERVAL_MS) {
        sendPing(link);
      }
    }
    if (link.echoPending && elapsedUs(link.echoSentUs) > ECHO_TIMEOUT_US) {
      link.echoPending = false;
      publishAck(link.childId, link.echoSeq, IOC_WD_OP_ECHO, IOC_WD_ACK_REJECTED, ACK_DETAIL_TIMEOUT);
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
    if (link.acousticPending && now - link.acousticSentMs > APPLY_TIMEOUT_MS) {
      publishAck(link.childId, link.acousticSeq, IOC_WD_OP_ACOUSTIC, IOC_WD_ACK_REJECTED, ACK_DETAIL_TIMEOUT);
      link.acousticPending = false;
    }

    publishLink(link);
  }
}

/* -------------------------------------------- comandos vindos do CAN */

void applyStagedConfig(Link& link, uint8_t seq) {
  if (link.stagedMismatch) {
    // Um SET MODE desta edição pertencia a outro perfil: nada dela é aplicado.
    link.stagedMismatch = false;
    link.staged = link.current;
    link.stagedDirty = false;
    publishAck(link.childId, seq, IOC_WD_OP_APPLY, IOC_WD_ACK_REJECTED, ACK_DETAIL_PROFILE_MISMATCH);
    return;
  }
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
  // Eco de pesquisa. Caminho 0: o Node confirma na hora (ida e volta só no
  // CAN). Caminho 1: o Node manda um PING ao sensor pela sessão e confirma no
  // PONG, com a parte Wi-Fi medida por ele no detalhe da confirmação.
  if (cmd.opcode == IOC_WD_OP_ECHO) {
    if (cmd.arg[0] == 0) {
      publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_APPLIED, 0);
    } else if (!link->secure) {
      publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_NO_LINK, 0);
    } else if (link->echoPending) {
      publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_REJECTED, ACK_DETAIL_BUSY);
    } else {
      Payload_Heartbeat ping;
      memset(&ping, 0, sizeof(ping));
      ping.magic_header = NET_MAGIC_HEADER;
      ping.cmd_type = CMD_PING;
      ping.request_counter = link->txCounter + 1u;
      link->echoCounter = ping.request_counter;
      link->echoSeq = cmd.seq;
      link->echoSentUs = pesquisaMonoUs();
      link->echoPending = sendToSensor(*link, &ping, sizeof(ping));
      if (!link->echoPending) publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_NO_LINK, 0);
    }
    return;
  }

  if (isPanel(*link)) {
    publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_REJECTED, ACK_DETAIL_PROFILE_MISMATCH);
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
      // arg1 traz o perfil a que o nome do modo pertence: 1 (ROTATING) e 1
      // (LEVEL) são o mesmo número em sensores diferentes.
      if (cmd.arg[1] != 0 && link->profile != 0 && cmd.arg[1] != link->profile) {
        link->stagedMismatch = true;
        publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_REJECTED, ACK_DETAIL_PROFILE_MISMATCH);
        break;
      }
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
    case IOC_WD_OP_ACOUSTIC: {
      if (link->profile != PROFILE_ACOUSTIC_MEMS) {
        publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_REJECTED, ACK_DETAIL_PROFILE_MISMATCH);
        break;
      }
      Payload_AcousticControl ctrl;
      memset(&ctrl, 0, sizeof(ctrl));
      ctrl.magic_header = NET_MAGIC_HEADER;
      ctrl.cmd_type = CMD_ACOUSTIC_CONTROL;
      ctrl.request_counter = ++link->configCounter;
      ctrl.action = cmd.arg[0];
      ctrl.argument = ioc_wd_arg_u16(&cmd.arg[1]);
      if (sendToSensor(*link, &ctrl, sizeof(ctrl))) {
        link->acousticPending = true;
        link->acousticSeq = cmd.seq;
        link->acousticCounter = ctrl.request_counter;
        link->acousticSentMs = millis();
        publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_ACCEPTED, 0);
      } else {
        publishAck(link->childId, cmd.seq, cmd.opcode, IOC_WD_ACK_NO_LINK, 0);
      }
      break;
    }
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
  uint32_t sentMs = 0;       // envio do comando, para o tempo de resposta
  uint64_t sentUs = 0;       // o mesmo, em microssegundos (pesquisa)
  char tx[12] = {0};
  char action[20] = {0};
};

/*
 * Trem de ecos (pesquisa). ECHO <node>.<filho> <n> <período_ms> [LOCAL|SENSOR]
 * envia n comandos IOC_WD_OP_ECHO e mede, no relógio da Probe, do envio do
 * quadro 0x304 à confirmação remontada (0x380+N). LOCAL: só o caminho CAN do
 * plano de dados; SENSOR: inclui a ida e volta Wi-Fi ao sensor, que o Node
 * mede no relógio dele e informa à parte (wl_us).
 *   [GW] ECHO node=2 child=1 path=SENSOR n=1 seq=7 status=APPLIED rtt_us=14250 wl_us=11800 t_us=...
 *   [GW] ECHO_DONE node=2 child=1 path=SENSOR sent=30 received=30 lost=0 min_us=... avg_us=... max_us=...
 */
constexpr uint8_t PROBE_ECHO_PENDING = 8;
constexpr uint32_t PROBE_ECHO_TIMEOUT_US = 3000000;
constexpr uint16_t PROBE_ECHO_MAX = 10000;

struct ProbeEcho {
  bool active = false;
  uint8_t node = 0;
  uint8_t child = 0;
  bool viaSensor = false;
  uint16_t total = 0;
  uint16_t sent = 0;
  uint16_t received = 0;
  uint16_t lost = 0;
  uint32_t periodUs = 0;
  uint64_t nextUs = 0;
  uint64_t sumUs = 0;
  uint32_t minUs = 0xFFFFFFFFu;
  uint32_t maxUs = 0;
  struct { bool used; uint8_t seq; uint16_t n; uint64_t sentUs; } pend[PROBE_ECHO_PENDING];
};

ioc_wd_rx probeRx[32];
ProbePending probePending[PROBE_PENDING_LEN];
ProbeEcho probeEcho;
uint8_t probeSeq = 0;
void probeEchoAck(uint8_t node, uint8_t child, const ioc_wd_ack& ack);
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
    case IOC_WD_LINK_SLEEPING: return "SLEEPING";
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
  if (ack.status == IOC_WD_ACK_REJECTED && ack.detail == ACK_DETAIL_PROFILE_MISMATCH) return "PROFILE_MISMATCH";
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
    "BATT_PCT=%u BATT_MV=%u DTC=0x%04X DTC_COUNT=%u T_US=%llu\n",
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
    static_cast<unsigned>(t.dtc_code), static_cast<unsigned>(t.dtc_count),
    static_cast<unsigned long long>(pesquisaMonoUs()));
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

// Acrescenta texto à linha montada em `line`, sem ultrapassar o buffer.
struct LineBuilder {
  char* buf;
  size_t cap;
  size_t len;
  void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    len += static_cast<size_t>(n);
    if (len >= cap) len = cap - 1;
  }
  // Nível em centésimos de dB; ausente quando o modo não o calcula.
  void db(const char* key, int16_t cb) {
    if (cb != IOC_WD_CB_NA) add(" %s=%.2f", key, static_cast<double>(cb) / 100.0);
  }
};

const char* acousticEventName(uint8_t cls) {
  switch (cls) {
    case ACOUSTIC_EVENT_IMPACT: return "IMPACT";
    case ACOUSTIC_EVENT_CRACK: return "CRACK";
    case ACOUSTIC_EVENT_TONAL: return "TONAL";
    case ACOUSTIC_EVENT_TRANSIENT: return "TRANSIENT";
    default: return "NONE";
  }
}

const char* yesNo(bool value) {
  return value ? "YES" : "NO";
}

// Mesmas chaves do console USB do sensor acústico, para que a TUI interprete
// as duas origens com um único analisador.
void probePrintAcoustic(uint8_t node, uint8_t child, const ioc_wd_acoustic& a) {
  static char line[960];
  LineBuilder out{line, sizeof(line), 0};
  const bool spectrum = (a.flags & ACOUSTIC_FLAG_SPECTRUM_VALID) != 0;
  out.add("TEL NODE=%u CHILD=%u SEQ=%u GW_TS_MS=%lu PROFILE=ACOUSTIC MODE=%s ACQ=I2S WIN=%u "
          "RATE_EFF_HZ=%u INTERVAL_MS=%u COVERAGE_PCT=%u CPU_PCT=%u SPECTRUM_VALID=%s OVERLOAD=%s MIC_FAULT=%s",
          static_cast<unsigned>(node), static_cast<unsigned>(child), static_cast<unsigned>(a.seq),
          static_cast<unsigned long>(millis()), acousticModeName(a.mode), static_cast<unsigned>(a.fft_size),
          static_cast<unsigned>(a.rate_hz), static_cast<unsigned>(a.interval_ms),
          static_cast<unsigned>(a.coverage_pct), static_cast<unsigned>(a.cpu_pct), yesNo(spectrum),
          yesNo(a.flags & ACOUSTIC_FLAG_OVERLOAD), yesNo(a.flags & ACOUSTIC_FLAG_MIC_FAULT));
  out.db("LAEQ_DB", a.laeq_cb);
  out.db("LCEQ_DB", a.lceq_cb);
  out.db("LZEQ_DB", a.lzeq_cb);
  out.db("LZPEAK_DB", a.lzpeak_cb);
  out.db("LAF_DB", a.laf_cb);
  out.db("LAFMAX_DB", a.lafmax_cb);
  out.db("LAFMIN_DB", a.lafmin_cb);
  if (a.crest_x100 != IOC_WD_CB_NA) out.add(" CREST=%.2f", static_cast<double>(a.crest_x100) / 100.0);
  if (a.kurtosis_x100 != IOC_WD_CB_NA) out.add(" KURT=%.2f", static_cast<double>(a.kurtosis_x100) / 100.0);
  if (spectrum) {
    out.add(" PEAK_HZ=%.1f", static_cast<double>(finiteOrZero(a.peak_hz)));
    if (a.centroid_hz) out.add(" CENTROID_HZ=%u", static_cast<unsigned>(a.centroid_hz));
    out.add(" FLATNESS=%.4f", static_cast<double>(a.flatness_x10000) / 10000.0);
  }
  out.add(" OCT_DB=");
  for (unsigned b = 0; b < IOC_WD_ACOUSTIC_BANDS; ++b) {
    if (a.octave_cb[b] != IOC_WD_CB_NA) out.add("%s%.1f", b ? "," : "", static_cast<double>(a.octave_cb[b]) / 100.0);
    else out.add("%s-", b ? "," : "");
  }
  switch (a.mode) {
    case ACOUSTIC_MODE_LEVEL:
      out.db("LEQ_T_DB", a.leq_total_cb);
      out.db("L10_DB", a.l10_cb);
      out.db("L50_DB", a.l50_cb);
      out.db("L90_DB", a.l90_cb);
      out.add(" DOSE_NR15_PCT=%.1f DOSE_NHO01_PCT=%.1f ELAPSED_S=%lu DOSE_LIMIT=%s",
              static_cast<double>(a.dose_nr15_x10) / 10.0, static_cast<double>(a.dose_nho01_x10) / 10.0,
              static_cast<unsigned long>(a.elapsed_s), yesNo(a.flags & ACOUSTIC_FLAG_DOSE_LIMIT));
      break;
    case ACOUSTIC_MODE_MACHINE: {
      const char* baseline = (a.flags & ACOUSTIC_FLAG_BASELINE_READY)      ? "READY"
                             : (a.flags & ACOUSTIC_FLAG_BASELINE_LEARNING) ? "LEARNING"
                                                                             : "NONE";
      out.db("TONAL_DB", a.tonal_cb);
      out.add(" F0_HZ=%.2f HARMONICS=%u", static_cast<double>(finiteOrZero(a.f0_hz)),
              static_cast<unsigned>(a.harmonics));
      if (a.anomaly_x100 != IOC_WD_CB_NA) out.add(" ANOMALY_Z=%.2f", static_cast<double>(a.anomaly_x100) / 100.0);
      out.add(" BASELINE=%s BASELINE_PCT=%u ANOMALY=%s", baseline, static_cast<unsigned>(a.baseline_pct),
              yesNo(a.flags & ACOUSTIC_FLAG_ANOMALY));
      break;
    }
    case ACOUSTIC_MODE_EVENTS:
      out.add(" EVENTS=%u EVENTS_INTERVAL=%u EVENT_ACTIVE=%s", static_cast<unsigned>(a.event_count),
              static_cast<unsigned>(a.events_interval), yesNo(a.flags & ACOUSTIC_FLAG_EVENT_ACTIVE));
      out.db("STA_LTA_DB", a.sta_lta_cb);
      out.db("BACKGROUND_DB", a.background_cb);
      if (a.event_age_s != ACOUSTIC_AGE_NONE) {
        out.add(" EVENT_CLASS=%s", acousticEventName(a.event_class));
        out.db("EVENT_PEAK_DB", a.event_peak_cb);
        out.db("EVENT_SEL_DB", a.event_sel_cb);
        out.add(" EVENT_DURATION_MS=%u", static_cast<unsigned>(a.event_duration_ms));
        if (a.event_centroid_hz) out.add(" EVENT_CENTROID_HZ=%u", static_cast<unsigned>(a.event_centroid_hz));
        out.add(" EVENT_AGE_S=%u", static_cast<unsigned>(a.event_age_s));
      }
      break;
    default:
      break;
  }
  out.add(" BATT_PCT=%u BATT_MV=%u DTC=0x%04X DTC_COUNT=%u", static_cast<unsigned>(a.battery_pct),
          static_cast<unsigned>(a.battery_mv), static_cast<unsigned>(a.dtc_code),
          static_cast<unsigned>(a.dtc_count));
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
      // RTT_MS: do envio do comando pela Probe até a chegada desta confirmação.
      Serial.printf("ACK COMMAND=%s STATE=%s TX=%s NODE=%u CHILD=%u DETAIL=%u%s%s RTT_MS=%lu RTT_US=%lu T_US=%llu\n",
                    pending.action, ackStateName(ack.status), pending.tx,
                    static_cast<unsigned>(node), static_cast<unsigned>(child),
                    static_cast<unsigned>(ack.detail), reason[0] ? " REASON=" : "", reason,
                    static_cast<unsigned long>(millis() - pending.sentMs),
                    static_cast<unsigned long>(pesquisaMonoUs() - pending.sentUs),
                    static_cast<unsigned long long>(pesquisaMonoUs()));
    }
    // Um SET fica concluído ao ser aceito pelo Node; os demais aguardam o resultado final.
    if (final || !pending.report) pending.used = false;
    return;
  }
}

void probePrintPanel(uint8_t node, uint8_t child, const uint8_t* data, size_t len) {
  if (len < 1) return;
  static char line[420];
  LineBuilder out{line, sizeof(line), 0};
  if (data[0] == IOC_PANEL_CMD_STATUS) {
    ioc_panel_status st;
    if (!ioc_panel_status_decode_body(data + 1, len - 1, &st)) return;
    const char* state = (st.flags & IOC_PANEL_ST_POWER_OFF) ? "OFF" : (st.flags & IOC_PANEL_ST_SLEEP) ? "SLEEP" : "AWAKE";
    out.add("PANEL NODE=%u CHILD=%u TYPE=STATUS STATE=%s SUPPLY=%s SLEEP_S=%u LEVEL=%u PAGE=0x%08lX PAGE_SEQ=%u "
            "REFRESHES=%u UPTIME_S=%lu CLOCK=%s KEYBOARD=%s",
            node, child, state, (st.flags & IOC_PANEL_ST_USB) ? "USB" : "BATTERY", st.sleep_s, st.severity,
            static_cast<unsigned long>(st.page_hash), st.page_seq, st.refreshes, static_cast<unsigned long>(st.uptime_s),
            yesNo(st.flags & IOC_PANEL_ST_CLOCK), yesNo(st.flags & IOC_PANEL_ST_KEYBOARD));
    if (st.flags & IOC_PANEL_ST_ALERT) out.add(" ALERT=UNACKED");
    if (st.battery_pct != 255) out.add(" BAT_PCT=%u BAT_MV=%u", st.battery_pct, st.battery_mv);
    if (st.temp_c10 != IOC_PANEL_NA_I16) out.add(" TEMP_C=%.1f", static_cast<double>(st.temp_c10) / 10.0);
    if (st.rh_10 != IOC_PANEL_NA_U16) out.add(" RH_PCT=%.1f", static_cast<double>(st.rh_10) / 10.0);
  } else if (data[0] == IOC_PANEL_CMD_MARK) {
    ioc_panel_mark mark;
    if (!ioc_panel_mark_decode_body(data + 1, len - 1, &mark)) return;
    out.add("PANEL NODE=%u CHILD=%u TYPE=MARK SEQ=%u KIND=%s STEP=%u AGE_MS=%lu TEXT_HEX=", node, child, mark.seq,
            ioc_panel_mark_kind_name(mark.kind), mark.step, static_cast<unsigned long>(mark.age_ms));
    for (uint8_t i = 0; i < mark.text_len; ++i) out.add("%02X", static_cast<unsigned>(static_cast<uint8_t>(mark.text[i])));
  } else {
    return;
  }
  Serial.println(line);
}

void probeHandleTransfer(uint8_t node, const ioc_wd_rx_slot& slot) {
  const uint8_t child = slot.child;
  switch (slot.kind) {
    case IOC_WD_KIND_PANEL:
      probePrintPanel(node, child, slot.data, slot.total_len);
      break;
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
    case IOC_WD_KIND_ACOUSTIC: {
      ioc_wd_acoustic a;
      if (ioc_wd_acoustic_decode(slot.data, slot.total_len, &a)) probePrintAcoustic(node, child, a);
      break;
    }
    case IOC_WD_KIND_CMD_ACK: {
      ioc_wd_ack ack;
      if (ioc_wd_ack_decode(slot.data, slot.total_len, &ack)) {
        if (ack.opcode == IOC_WD_OP_ECHO) probeEchoAck(node, child, ack);
        else probePrintAck(node, child, ack);
      }
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
        // Tempo de ida e volta Node <-> sensor em décimos de milissegundo; -1 = sem medida.
        char rtt[12] = "-1";
        char rttMax[12] = "-1";
        if (link.rtt_100us != IOC_WD_RTT_UNKNOWN) {
          snprintf(rtt, sizeof(rtt), "%u.%u", static_cast<unsigned>(link.rtt_100us / 10u),
                   static_cast<unsigned>(link.rtt_100us % 10u));
        }
        if (link.rtt_max_100us != IOC_WD_RTT_UNKNOWN) {
          snprintf(rttMax, sizeof(rttMax), "%u.%u", static_cast<unsigned>(link.rtt_max_100us / 10u),
                   static_cast<unsigned>(link.rtt_max_100us % 10u));
        }
        Serial.printf("[GW] WIRELESS_LINK node=%u child=%u state=%s rssi=%d age_s=%u rx=%u "
                      "auth_fail=%u replay=%u lost=%u stream=%s period_ms=%u rtt_ms=%s rtt_max_ms=%s t_us=%llu\n",
                      static_cast<unsigned>(node), static_cast<unsigned>(child),
                      linkStateName(link.state), static_cast<int>(link.wifi_rssi),
                      static_cast<unsigned>(link.session_age_s), static_cast<unsigned>(link.rx_datagrams),
                      static_cast<unsigned>(link.auth_failures), static_cast<unsigned>(link.replay_drops),
                      static_cast<unsigned>(link.lost_datagrams), link.stream_enabled ? "ON" : "OFF",
                      static_cast<unsigned>(link.stream_period_ms), rtt, rttMax,
                      static_cast<unsigned long long>(pesquisaMonoUs()));
      }
      break;
    }
    case IOC_WD_KIND_LINKSTATS: {
      // Medidas de pesquisa no relógio do Node: -1 = sem amostra na janela.
      ioc_wd_linkstats st;
      if (ioc_wd_linkstats_decode(slot.data, slot.total_len, &st)) {
        auto num = [](uint16_t v) { return v == IOC_WD_RTT_UNKNOWN ? -1L : static_cast<long>(v); };
        auto tenth = [](uint16_t v) { return v == IOC_WD_RTT_UNKNOWN ? -1L : static_cast<long>(v) * 100L; };
        Serial.printf("[GW] WIRELESS_STATS node=%u child=%u crypto_rx_us=%ld crypto_tx_us=%ld crypto_max_us=%ld "
                      "handshake_us=%ld bridge_avg_us=%ld bridge_max_us=%ld t_us=%llu\n",
                      static_cast<unsigned>(node), static_cast<unsigned>(child), num(st.crypto_rx_us),
                      num(st.crypto_tx_us), num(st.crypto_max_us), num(st.handshake_us),
                      tenth(st.bridge_avg_100us), tenth(st.bridge_max_100us),
                      static_cast<unsigned long long>(pesquisaMonoUs()));
      }
      break;
    }
    case IOC_WD_KIND_CONFIG: {
      ioc_wd_config cfg;
      if (ioc_wd_config_decode(slot.data, slot.total_len, &cfg)) {
        // O perfil vai no fim: leitores antigos continuam encontrando as mesmas chaves.
        // Um Node v0.18 não o informa, e então os nomes são os da vibração.
        Serial.printf("[GW] WIRELESS_CONFIG node=%u child=%u status=%s mode=%s rate_req_hz=%.2f "
                      "rate_eff_hz=%.2f window=%s window_size=%u stalta=%.3f gain=%.4f%s%s\n",
                      static_cast<unsigned>(node), static_cast<unsigned>(child),
                      cfg.status == 1 ? "APPLIED" : (cfg.status == 2 ? "REJECTED" : "CURRENT"),
                      modeNameFor(cfg.profile, cfg.mode), static_cast<double>(finiteOrZero(cfg.rate_req_hz)),
                      static_cast<double>(finiteOrZero(cfg.rate_eff_hz)), windowName(cfg.window_type),
                      static_cast<unsigned>(cfg.window_size), static_cast<double>(finiteOrZero(cfg.stalta)),
                      static_cast<double>(finiteOrZero(cfg.gain)), cfg.profile ? " profile=" : "",
                      cfg.profile ? profileTag(cfg.profile) : "");
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
  pending.sentMs = millis();
  pending.sentUs = pesquisaMonoUs();
  strncpy(pending.tx, tx, sizeof(pending.tx) - 1);
  pending.tx[sizeof(pending.tx) - 1] = '\0';
  strncpy(pending.action, action, sizeof(pending.action) - 1);
  pending.action[sizeof(pending.action) - 1] = '\0';

  CANMessage msg;
  msg.id = CAN_ID_WIRELESS_DATA_CMD;
  msg.len = IOC_WD_FRAME_LEN;
  ioc_wd_command_encode(&cmd, msg.data);
  if (canSend(msg)) netMetricsOnTransmit(msg);
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
  static const char* const ACOUSTIC_MODES[] = {"IDLE", "LEVEL", "MACHINE", "EVENTS"};
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

  // Sensor acústico: reinício da medição e assinatura de referência (MACHINE).
  uint8_t acousticAction = 0;
  if (strcasecmp(action, "MEASURE_RESET") == 0) acousticAction = ACOUSTIC_CTRL_MEASURE_RESET;
  else if (strcasecmp(action, "BASELINE_LEARN") == 0) acousticAction = ACOUSTIC_CTRL_BASELINE_LEARN;
  else if (strcasecmp(action, "BASELINE_CLEAR") == 0) acousticAction = ACOUSTIC_CTRL_BASELINE_CLEAR;
  if (acousticAction != 0) {
    uint16_t argument = 0;
    if (acousticAction == ACOUSTIC_CTRL_BASELINE_LEARN) {
      const char* secondsText = findValue(tokens, count, "SECONDS");
      const unsigned long seconds = secondsText ? strtoul(secondsText, nullptr, 10) : 60ul;
      if (seconds < 5ul || seconds > 3600ul) {
        probeReject(action, tx, "INVALID_FIELD");
        return;
      }
      argument = static_cast<uint16_t>(seconds);
    }
    arg[0] = acousticAction;
    ioc_wd_put_arg_u16(&arg[1], argument);
    probeSendCommand(n, c, IOC_WD_OP_ACOUSTIC, arg, tx, action, true);
    return;
  }

  if (strcasecmp(action, "CONFIG") == 0) {
    const char* modeText = findValue(tokens, count, "MODE");
    const char* rateText = findValue(tokens, count, "RATE_HZ");
    if (!rateText) rateText = findValue(tokens, count, "RATE");
    const char* windowText = findValue(tokens, count, "WINDOW");
    const char* sizeText = findValue(tokens, count, "WINDOW_SIZE");
    if (!sizeText) sizeText = findValue(tokens, count, "FFT_SIZE");
    const char* staltaText = findValue(tokens, count, "STALTA");
    if (!staltaText) staltaText = findValue(tokens, count, "THRESHOLD");
    const char* gainText = findValue(tokens, count, "GAIN");

    // O nome escolhe o perfil: o Node recusa um modo de vibração para o
    // microfone (e vice-versa) em vez de aplicar o número de outro sensor.
    int mode = lookupName(modeText, MODES, 4);
    uint8_t modeProfile = PROFILE_ACCELEROMETER;
    if (mode < 0) {
      mode = lookupName(modeText, ACOUSTIC_MODES, 4);
      modeProfile = PROFILE_ACOUSTIC_MEMS;
    }
    const int window = lookupName(windowText, WINDOWS, 5);
    if ((modeText && mode < 0) || (windowText && window < 0)) {
      probeReject(action, tx, "INVALID_FIELD");
      return;
    }
    if (mode >= 0) {
      memset(arg, 0, 4);
      arg[0] = static_cast<uint8_t>(mode);
      arg[1] = mode == 0 ? 0 : modeProfile; // IDLE vale para os dois perfis
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


/* ------------------------------------------------- trem de ecos (pesquisa) */

void probeEchoFinish() {
  ProbeEcho& e = probeEcho;
  const uint32_t avg = e.received ? static_cast<uint32_t>(e.sumUs / e.received) : 0u;
  Serial.printf("[GW] ECHO_DONE node=%u child=%u path=%s sent=%u received=%u lost=%u min_us=%ld avg_us=%lu "
                "max_us=%lu t_us=%llu\n",
                static_cast<unsigned>(e.node), static_cast<unsigned>(e.child), e.viaSensor ? "SENSOR" : "LOCAL",
                static_cast<unsigned>(e.sent), static_cast<unsigned>(e.received), static_cast<unsigned>(e.lost),
                e.received ? static_cast<long>(e.minUs) : -1L, static_cast<unsigned long>(avg),
                static_cast<unsigned long>(e.maxUs), static_cast<unsigned long long>(pesquisaMonoUs()));
  e.active = false;
}

void probeEchoAck(uint8_t node, uint8_t child, const ioc_wd_ack& ack) {
  ProbeEcho& e = probeEcho;
  const uint64_t now = pesquisaMonoUs();
  for (auto& p : e.pend) {
    if (!p.used || p.seq != ack.seq || node != e.node || child != e.child) continue;
    p.used = false;
    const uint32_t rtt = static_cast<uint32_t>(now - p.sentUs);
    // LOCAL: qualquer confirmação fecha a ida e volta no CAN (sem sensor
    // associado o Node responde UNKNOWN pelo mesmo caminho). SENSOR: só o PONG.
    const bool ok = e.viaSensor ? ack.status == IOC_WD_ACK_APPLIED : true;
    long wl = -1;
    if (ok && e.viaSensor) wl = static_cast<long>(ack.detail) * 100L;
    Serial.printf("[GW] ECHO node=%u child=%u path=%s n=%u seq=%u status=%s rtt_us=%ld wl_us=%ld t_us=%llu\n",
                  static_cast<unsigned>(node), static_cast<unsigned>(child), e.viaSensor ? "SENSOR" : "LOCAL",
                  static_cast<unsigned>(p.n), static_cast<unsigned>(ack.seq), ackStateName(ack.status),
                  ok ? static_cast<long>(rtt) : -1L, wl, static_cast<unsigned long long>(now));
    if (ok) {
      ++e.received;
      e.sumUs += rtt;
      if (rtt < e.minUs) e.minUs = rtt;
      if (rtt > e.maxUs) e.maxUs = rtt;
    } else {
      ++e.lost;
    }
    return;
  }
}

void probeEchoPoll() {
  ProbeEcho& e = probeEcho;
  if (!e.active) return;
  const uint64_t now = pesquisaMonoUs();
  bool waiting = false;
  for (auto& p : e.pend) {
    if (!p.used) continue;
    if (now - p.sentUs > PROBE_ECHO_TIMEOUT_US) {
      p.used = false;
      ++e.lost;
      Serial.printf("[GW] ECHO node=%u child=%u path=%s n=%u seq=%u status=TIMEOUT rtt_us=-1 wl_us=-1 t_us=%llu\n",
                    static_cast<unsigned>(e.node), static_cast<unsigned>(e.child), e.viaSensor ? "SENSOR" : "LOCAL",
                    static_cast<unsigned>(p.n), static_cast<unsigned>(p.seq), static_cast<unsigned long long>(now));
    } else {
      waiting = true;
    }
  }
  if (e.sent < e.total && static_cast<int64_t>(now - e.nextUs) >= 0) {
    auto* slot = &e.pend[0];
    bool free = false;
    for (auto& p : e.pend) {
      if (!p.used) { slot = &p; free = true; break; }
    }
    if (free) {
      ioc_wd_command cmd;
      cmd.seq = ++probeSeq;
      cmd.node_id = e.node;
      cmd.child_id = e.child;
      cmd.opcode = IOC_WD_OP_ECHO;
      memset(cmd.arg, 0, sizeof(cmd.arg));
      cmd.arg[0] = e.viaSensor ? 1 : 0;
      CANMessage msg;
      msg.id = CAN_ID_WIRELESS_DATA_CMD;
      msg.len = IOC_WD_FRAME_LEN;
      ioc_wd_command_encode(&cmd, msg.data);
      // O instante é tomado antes de entregar o quadro: a espera na fila da
      // Probe e a arbitragem fazem parte da medida.
      slot->sentUs = pesquisaMonoUs();
      if (canSend(msg)) {
        netMetricsOnTransmit(msg);
        slot->used = true;
        slot->seq = cmd.seq;
        slot->n = static_cast<uint16_t>(e.sent + 1u);
        ++e.sent;
        waiting = true;
      }
      e.nextUs = now + e.periodUs;
    }
  }
  if (e.sent >= e.total && !waiting) probeEchoFinish();
}

bool probeEchoCommand(const char* command) {
  // ECHO <node>.<filho> <n> <período_ms> [LOCAL|SENSOR] | ECHO CANCEL
  char line[64];
  strncpy(line, command + 4, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';
  char* save = nullptr;
  char* target = strtok_r(line, " \t", &save);
  if (target && strcasecmp(target, "CANCEL") == 0) {
    if (probeEcho.active) probeEchoFinish();
    return true;
  }
  char* countText = strtok_r(nullptr, " \t", &save);
  char* periodText = strtok_r(nullptr, " \t", &save);
  char* pathText = strtok_r(nullptr, " \t", &save);
  unsigned node = 0, child = 0;
  const unsigned long count = countText ? strtoul(countText, nullptr, 10) : 0;
  const unsigned long period = periodText ? strtoul(periodText, nullptr, 10) : 0;
  const bool viaSensor = pathText && strcasecmp(pathText, "SENSOR") == 0;
  if (!target || sscanf(target, "%u.%u", &node, &child) != 2 || node == 0 || node >= 32 || child == 0 ||
      child > IOC_WD_MAX_CHILD || count == 0 || count > PROBE_ECHO_MAX || period < 20 || period > 60000 ||
      (pathText && !viaSensor && strcasecmp(pathText, "LOCAL") != 0)) {
    Serial.println("[GW] ECHO_ERROR use: ECHO <node>.<filho> <n 1..10000> <periodo_ms 20..60000> [LOCAL|SENSOR] | ECHO CANCEL");
    return true;
  }
  if (probeEcho.active) probeEchoFinish();
  probeEcho = ProbeEcho{};
  probeEcho.active = true;
  probeEcho.node = static_cast<uint8_t>(node);
  probeEcho.child = static_cast<uint8_t>(child);
  probeEcho.viaSensor = viaSensor;
  probeEcho.total = static_cast<uint16_t>(count);
  probeEcho.periodUs = static_cast<uint32_t>(period) * 1000u;
  probeEcho.nextUs = pesquisaMonoUs();
  for (auto& p : probeEcho.pend) p.used = false;
  Serial.printf("[GW] ECHO_START node=%u child=%u path=%s count=%lu period_ms=%lu t_us=%llu\n", node, child,
                viaSensor ? "SENSOR" : "LOCAL", count, period, static_cast<unsigned long long>(pesquisaMonoUs()));
  return true;
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

  ioc_pcache_init(&panelCache);
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
  updatePanelMailbox();

  ioc_wd_link report;
  memset(&report, 0, sizeof(report));
  report.state = IOC_WD_LINK_DOWN;
  report.rtt_100us = IOC_WD_RTT_UNKNOWN;       // sem sessão não há medida (antes saía 0,0 ms)
  report.rtt_max_100us = IOC_WD_RTT_UNKNOWN;
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

bool wirelessLinkIsPresent(uint64_t uuid) {
  if (NODE_ID == 0) return false;
  const Link* link = findLinkByUuid(uuid);
  if (!link) return false;
  if (link->secure) return true;
  return isPanel(*link) && link->panelAsleep && static_cast<int32_t>(millis() - link->panelPresentUntilMs) <= 0;
}

void wirelessLinkTap(const CANMessage& msg, bool own) {
  if (NODE_ID == 0) return;
  tapFrame(msg, own);
}

bool canSend(const CANMessage& msg) {
  if (!can.tryToSend(msg)) return false;
  wirelessLinkTap(msg, true);
  return true;
}

bool wirelessLinkIsSecure(uint64_t uuid) {
  if (NODE_ID == 0) return false;
  const Link* link = findLinkByUuid(uuid);
  return link && link->secure;
}

/* ------------------------------------------------ ensaios (fault_injection) */

void wirelessLinkInjectDropSessions() {
  if (NODE_ID == 0) return;
  for (uint8_t i = 0; i < MAX_LINKS; ++i) {
    Link& link = links[i];
    if (!link.used) continue;
    if (link.pending.active) {
      ioc_secure_zero(link.pending.sessionKey, IOC_LINK_KEY_LEN);
      link.pending.active = false;
    }
    if (!link.secure) continue;
    Serial.printf("[NODE %u] [WLINK] child=%u sessao descartada (ensaio)\n", NODE_ID,
                  static_cast<unsigned>(link.childId));
    dropSession(link);
    publishLink(link, true);
  }
}

void wirelessLinkInjectApOutage(uint32_t durationMs) {
  if (NODE_ID == 0) return;
  if (durationMs == 0) {
    if (apOutageActive) {
      apOutageActive = false;
      if (usedLinkCount() > 0) startAccessPoint();
    }
    return;
  }
  wirelessLinkInjectDropSessions();
  stopOfferAdvertising();
  stopAccessPoint();
  apOutageActive = true;
  apOutageStartMs = millis();
  apOutageDurationMs = durationMs;
}

void wirelessLinkRadioOff() {
  if (NODE_ID == 0) return;
  for (uint8_t i = 0; i < MAX_LINKS; ++i) {
    if (links[i].used && links[i].secure) dropSession(links[i]);
  }
  stopOfferAdvertising();
  stopAccessPoint();
  radioOff = true;
}

void wirelessLinkPoll() {
  if (NODE_ID == 0) {
    probeEchoPoll();
    return;
  }
  if (apOutageActive && millis() - apOutageStartMs >= apOutageDurationMs) {
    wirelessLinkInjectApOutage(0);
  }
  // Há vínculos, mas o ponto de acesso não está no ar (a ativação falhou):
  // sem nova tentativa os sensores ficariam sem caminho até o próximo vínculo.
  if (!apRunning && haveKey && !apOutageActive && !radioOff && !apStopPending && usedLinkCount() > 0 &&
      millis() - lastApAttemptMs >= AP_RETRY_MS) {
    startAccessPoint();
  }
  printStationEvents();
  receiveDatagrams();
  maintainLinks();
  for (uint8_t i = 0; i < MAX_LINKS; ++i) {
    if (links[i].used) tapFlush(links[i], false);
  }
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
    const ioc_wd_rx_result result = ioc_wd_rx_push(&probeRx[node], rx.data, millis(), &done);
    if (result == IOC_WD_RX_COMPLETE && done) {
      netMetricsOnTransfer(true);
      probeHandleTransfer(node, *done);
    } else if (result == IOC_WD_RX_ERROR) {
      netMetricsOnTransfer(false);
    }
    return true;
  }

  return false;
}

bool wirelessLinkHandleSerialCommand(const char* command) {
  if (NODE_ID != 0 || !command) return false;
  if (strncasecmp(command, "ECHO ", 5) == 0) return probeEchoCommand(command);
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
