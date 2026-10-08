#include "fault_injection.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "can_ids.h"
#include "net_metrics.h"
#include "node_config.h"
#include "protocolo.h"
#include "wireless_link.h"

#include "ioc_canbits.h"

extern ACAN2515 can;

namespace {

constexpr uint32_t MIN_OUTAGE_MS = 1000;
constexpr uint8_t  LOAD_MAX_PERCENT = 90;
constexpr uint8_t  LOAD_DRIVER_WATERMARK = 8;   // quadros de carga aguardando no driver
constexpr uint32_t LOAD_MAX_CREDIT_US = 20000;  // crédito máximo acumulado entre duas passagens do laço
constexpr uint32_t LOAD_MAX_CREDIT_MILLIBITS = 8u * 135u * 1000u; // sem rajada quando o driver não acompanha

const char* kindName(uint8_t kind) {
  switch (kind) {
    case TEST_KIND_POWER_CYCLE:  return "POWER_CYCLE";
    case TEST_KIND_SILENCE:      return "SILENCE";
    case TEST_KIND_AP_OUTAGE:    return "AP_OUTAGE";
    case TEST_KIND_DROP_SESSION: return "DROP_SESSION";
    case TEST_KIND_BUS_LOAD:     return "BUS_LOAD";
    case TEST_KIND_CANCEL:       return "CANCEL";
    default:                     return "UNKNOWN";
  }
}

const char* stateName(uint8_t state) {
  switch (state) {
    case TEST_STATE_STARTED:  return "STARTED";
    case TEST_STATE_ENDED:    return "ENDED";
    case TEST_STATE_REJECTED: return "REJECTED";
    default:                  return "UNKNOWN";
  }
}

uint32_t clampDuration(uint32_t ms) {
  if (ms < MIN_OUTAGE_MS) return MIN_OUTAGE_MS;
  if (ms > TEST_MAX_DURATION_MS) return TEST_MAX_DURATION_MS;
  return ms;
}

/* ===================================================================== */
/* Node funcional                                                        */
/* ===================================================================== */

#if IOT_FAULT_INJECTION

enum class NodeFault : uint8_t { None, PowerCycle, Silence, ApOutage };

NodeFault nodeFault = NodeFault::None;
uint8_t nodeFaultSeq = 0;
uint32_t nodeFaultStartMs = 0;
uint32_t nodeFaultDurationMs = 0;

void sendNodeStatus(uint8_t seq, uint8_t kind, uint8_t state, uint32_t durationMs) {
  const uint16_t units = static_cast<uint16_t>(durationMs / 100u);
  CANMessage msg;
  msg.id = CAN_ID_TEST_STATUS;
  msg.len = 8;
  msg.data[0] = seq;
  msg.data[1] = NODE_ID;
  msg.data[2] = kind;
  msg.data[3] = state;
  msg.data[4] = static_cast<uint8_t>(units & 0xFFu);
  msg.data[5] = static_cast<uint8_t>(units >> 8);
  msg.data[6] = 0;
  msg.data[7] = TEST_MAGIC_STATUS;
  can.tryToSend(msg);
  Serial.printf("[NODE %u] [ENSAIO] %s %s duracao=%lu ms\n", NODE_ID, kindName(kind), stateName(state),
                static_cast<unsigned long>(durationMs));
}

// Dá tempo para a confirmação sair antes de o Node se calar.
void flushCanTransmit() {
  const uint32_t start = millis();
  while (can.transmitBufferCount(0) > 0 && millis() - start < 50) delay(1);
  delay(10);
}

// O MCP2515 só troca de modo quando a transmissão em curso termina. A
// biblioteca espera 2 ms e devolve erro, mas o pedido continua valendo no
// controlador. Por isso o retorno ao modo normal é pedido sempre, conferido e
// repetido: um Node não pode ficar mudo depois de um ensaio.
bool normalModePending = false;
uint32_t lastNormalModeAttemptMs = 0;

void enterBusSilence() {
  // Em modo somente escuta o controlador não transmite nem confirma quadros,
  // como um Node desligado. Se a troca de modo falhar, o Node apenas deixa
  // de transmitir (continua confirmando quadros dos demais).
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    if (can.changeModeOnTheFly(ACAN2515Settings::ListenOnlyMode) == 0) break;
    delay(2);
  }
}

bool requestNormalMode() {
  lastNormalModeAttemptMs = millis();
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    if (can.changeModeOnTheFly(ACAN2515Settings::NormalMode) == 0) return true;
    delay(2);
  }
  return false;
}

void leaveBusSilence() {
  normalModePending = !requestNormalMode();
  if (normalModePending) {
    Serial.printf("[NODE %u] [ENSAIO] retorno ao modo normal pendente; nova tentativa em curso\n", NODE_ID);
  }
}

void endNodeFault(bool cancelled) {
  const NodeFault previous = nodeFault;
  const uint32_t elapsed = millis() - nodeFaultStartMs;
  nodeFault = NodeFault::None;

  if (previous == NodeFault::PowerCycle) {
    // Os rádios já foram desligados: o cancelamento apenas antecipa o reinício.
    Serial.printf("[NODE %u] [ENSAIO] POWER_CYCLE: reiniciando\n", NODE_ID);
    Serial.flush();
    ESP.restart();
    return; // no host o reinício é apenas registrado
  }
  if (previous == NodeFault::Silence) {
    leaveBusSilence();
    sendNodeStatus(nodeFaultSeq, TEST_KIND_SILENCE, TEST_STATE_ENDED, elapsed);
  } else if (previous == NodeFault::ApOutage) {
    if (cancelled) wirelessLinkInjectApOutage(0);
    sendNodeStatus(nodeFaultSeq, TEST_KIND_AP_OUTAGE, TEST_STATE_ENDED, elapsed);
  }
}

void handleNodeCommand(const CANMessage& rx) {
  if (rx.len != 8 || rx.data[7] != TEST_MAGIC_CMD) return;
  if (rx.data[1] != NODE_ID) return;

  const uint8_t seq = rx.data[0];
  const uint8_t kind = rx.data[2];
  const uint32_t requestedMs = (static_cast<uint32_t>(rx.data[4]) | (static_cast<uint32_t>(rx.data[5]) << 8)) * 100u;

  if (kind == TEST_KIND_CANCEL) {
    if (nodeFault != NodeFault::None) endNodeFault(true);
    return;
  }
  // Um ensaio por vez; durante o silêncio só o cancelamento é ouvido.
  if (nodeFault != NodeFault::None) {
    if (!faultInjectionSilent()) sendNodeStatus(seq, kind, TEST_STATE_REJECTED, 0);
    return;
  }

  const uint32_t durationMs = clampDuration(requestedMs);
  switch (kind) {
    case TEST_KIND_POWER_CYCLE:
    case TEST_KIND_SILENCE:
      sendNodeStatus(seq, kind, TEST_STATE_STARTED, durationMs);
      flushCanTransmit();
      if (kind == TEST_KIND_POWER_CYCLE) wirelessLinkRadioOff();
      enterBusSilence();
      nodeFault = kind == TEST_KIND_POWER_CYCLE ? NodeFault::PowerCycle : NodeFault::Silence;
      break;
    case TEST_KIND_AP_OUTAGE:
      sendNodeStatus(seq, kind, TEST_STATE_STARTED, durationMs);
      wirelessLinkInjectApOutage(durationMs);
      nodeFault = NodeFault::ApOutage;
      break;
    case TEST_KIND_DROP_SESSION:
      sendNodeStatus(seq, kind, TEST_STATE_STARTED, 0);
      wirelessLinkInjectDropSessions();
      sendNodeStatus(seq, kind, TEST_STATE_ENDED, 0);
      return;
    default:
      sendNodeStatus(seq, kind, TEST_STATE_REJECTED, 0);
      return;
  }
  nodeFaultSeq = seq;
  nodeFaultStartMs = millis();
  nodeFaultDurationMs = durationMs;
}

#endif // IOT_FAULT_INJECTION

/* ===================================================================== */
/* Probe 00                                                              */
/* ===================================================================== */

uint8_t probeSeq = 0;

struct LoadGenerator {
  bool active = false;
  uint8_t percent = 0;
  bool highPriority = true;
  uint32_t startMs = 0;
  uint32_t durationMs = 0;
  uint32_t lastUs = 0;
  uint32_t creditBits = 0;      // em milésimos de bit
  uint32_t frames = 0;
  uint64_t bits = 0;
  uint16_t counter = 0;
};

LoadGenerator load;

void probeSendCommand(uint8_t node, uint8_t kind, uint32_t durationMs, uint8_t arg0 = 0, uint8_t arg1 = 0) {
  const uint16_t units = static_cast<uint16_t>(durationMs / 100u);
  CANMessage msg;
  msg.id = CAN_ID_TEST_CMD;
  msg.len = 8;
  msg.data[0] = ++probeSeq;
  msg.data[1] = node;
  msg.data[2] = kind;
  msg.data[3] = arg0;
  msg.data[4] = static_cast<uint8_t>(units & 0xFFu);
  msg.data[5] = static_cast<uint8_t>(units >> 8);
  msg.data[6] = arg1;
  msg.data[7] = TEST_MAGIC_CMD;
  if (can.tryToSend(msg)) netMetricsOnTransmit(msg);
  Serial.printf("[GW] FAULT_TX kind=%s node=%u duration_ms=%lu seq=%u\n", kindName(kind),
                static_cast<unsigned>(node), static_cast<unsigned long>(durationMs),
                static_cast<unsigned>(probeSeq));
}

void probePrintLoadEvent(uint8_t state) {
  const uint32_t elapsed = millis() - load.startMs;
  // Ocupação efetivamente gerada, em centésimos de ponto percentual.
  uint32_t achieved = 0;
  if (elapsed > 0) {
    achieved = static_cast<uint32_t>((load.bits * 1000000ull) /
                                     (static_cast<uint64_t>(CAN_NOMINAL_BITRATE) * elapsed / 10ull));
  }
  Serial.printf("[GW] FAULT_EVENT node=0 kind=BUS_LOAD state=%s duration_ms=%lu seq=%u load_pct=%u "
                "priority=%s frames=%lu achieved_pct=%lu.%02lu\n",
                stateName(state),
                static_cast<unsigned long>(state == TEST_STATE_STARTED ? load.durationMs : elapsed),
                static_cast<unsigned>(probeSeq), static_cast<unsigned>(load.percent),
                load.highPriority ? "HIGH" : "LOW", static_cast<unsigned long>(load.frames),
                static_cast<unsigned long>(achieved / 100u), static_cast<unsigned long>(achieved % 100u));
}

void probeStopLoad() {
  if (!load.active) return;
  load.active = false;
  probePrintLoadEvent(TEST_STATE_ENDED);
}

void probeStartLoad(uint8_t percent, uint32_t durationMs, bool highPriority) {
  if (load.active) probeStopLoad();
  load = LoadGenerator{};
  load.active = true;
  load.percent = percent;
  load.highPriority = highPriority;
  load.startMs = millis();
  load.durationMs = durationMs;
  load.lastUs = micros();
  ++probeSeq;
  probePrintLoadEvent(TEST_STATE_STARTED);
}

void probePollLoad() {
  if (!load.active) return;
  if (millis() - load.startMs >= load.durationMs) {
    probeStopLoad();
    return;
  }

  const uint32_t nowUs = micros();
  uint32_t elapsedUs = nowUs - load.lastUs;
  load.lastUs = nowUs;
  if (elapsedUs > LOAD_MAX_CREDIT_US) elapsedUs = LOAD_MAX_CREDIT_US;
  // bits = taxa * tempo * percentual; mantido em milésimos de bit.
  load.creditBits += static_cast<uint32_t>(
      (static_cast<uint64_t>(CAN_NOMINAL_BITRATE) * elapsedUs * load.percent) / 100000ull);
  if (load.creditBits > LOAD_MAX_CREDIT_MILLIBITS) load.creditBits = LOAD_MAX_CREDIT_MILLIBITS;

  while (can.transmitBufferCount(0) < LOAD_DRIVER_WATERMARK) {
    CANMessage msg;
    msg.id = load.highPriority ? CAN_ID_TEST_LOAD_HIGH : CAN_ID_TEST_LOAD_LOW;
    msg.len = 8;
    msg.data[0] = static_cast<uint8_t>(load.counter >> 8);
    msg.data[1] = static_cast<uint8_t>(load.counter & 0xFFu);
    for (uint8_t i = 2; i < 8; ++i) msg.data[i] = (i & 1u) ? 0xAA : 0x55;
    const uint32_t frameBits = ioc_can_frame_bits(static_cast<uint16_t>(msg.id), msg.len, msg.data);
    if (load.creditBits < frameBits * 1000u) break;
    if (!can.tryToSend(msg)) break;
    load.creditBits -= frameBits * 1000u;
    load.bits += frameBits;
    ++load.frames;
    ++load.counter;
    netMetricsOnTransmit(msg);
  }
}

void probeHandleStatus(const CANMessage& rx) {
  if (rx.len != 8 || rx.data[7] != TEST_MAGIC_STATUS) return;
  const uint32_t durationMs = (static_cast<uint32_t>(rx.data[4]) | (static_cast<uint32_t>(rx.data[5]) << 8)) * 100u;
  Serial.printf("[GW] FAULT_EVENT node=%u kind=%s state=%s duration_ms=%lu seq=%u\n",
                static_cast<unsigned>(rx.data[1]), kindName(rx.data[2]), stateName(rx.data[3]),
                static_cast<unsigned long>(durationMs), static_cast<unsigned>(rx.data[0]));
}

void probeUsage() {
  Serial.println("[GW] FAULT_ERROR use: FAULT POWER_CYCLE|SILENCE|AP_OUTAGE <node> <segundos> | "
                 "FAULT DROP_SESSION <node> | FAULT LOAD <pct> <segundos> [HIGH|LOW] | FAULT CANCEL [node]");
}

void probeHandleLine(char* tokens[], uint8_t count) {
  if (count == 0) {
    probeUsage();
    return;
  }
  const char* verb = tokens[0];

  if (strcasecmp(verb, "CANCEL") == 0) {
    probeStopLoad();
    if (count >= 2) {
      const unsigned long node = strtoul(tokens[1], nullptr, 10);
      if (node == 0 || node >= 32) {
        Serial.println("[GW] FAULT_ERROR node invalido; use 1..31");
        return;
      }
      probeSendCommand(static_cast<uint8_t>(node), TEST_KIND_CANCEL, 0);
    }
    return;
  }

  if (strcasecmp(verb, "LOAD") == 0) {
    if (count < 3) {
      probeUsage();
      return;
    }
    const unsigned long percent = strtoul(tokens[1], nullptr, 10);
    const unsigned long seconds = strtoul(tokens[2], nullptr, 10);
    bool high = true;
    if (count >= 4) {
      if (strcasecmp(tokens[3], "LOW") == 0) high = false;
      else if (strcasecmp(tokens[3], "HIGH") != 0) {
        probeUsage();
        return;
      }
    }
    if (percent == 0 || percent > LOAD_MAX_PERCENT || seconds == 0) {
      Serial.printf("[GW] FAULT_ERROR carga deve estar entre 1 e %u %% e a duracao ser maior que zero\n",
                    static_cast<unsigned>(LOAD_MAX_PERCENT));
      return;
    }
    probeStartLoad(static_cast<uint8_t>(percent), clampDuration(seconds * 1000ul), high);
    return;
  }

  uint8_t kind = 0;
  if (strcasecmp(verb, "POWER_CYCLE") == 0) kind = TEST_KIND_POWER_CYCLE;
  else if (strcasecmp(verb, "SILENCE") == 0) kind = TEST_KIND_SILENCE;
  else if (strcasecmp(verb, "AP_OUTAGE") == 0) kind = TEST_KIND_AP_OUTAGE;
  else if (strcasecmp(verb, "DROP_SESSION") == 0) kind = TEST_KIND_DROP_SESSION;
  else {
    probeUsage();
    return;
  }

  if (count < 2) {
    probeUsage();
    return;
  }
  const unsigned long node = strtoul(tokens[1], nullptr, 10);
  if (node == 0 || node >= 32) {
    Serial.println("[GW] FAULT_ERROR node invalido; use 1..31");
    return;
  }
  uint32_t durationMs = 0;
  if (kind != TEST_KIND_DROP_SESSION) {
    if (count < 3) {
      probeUsage();
      return;
    }
    const unsigned long seconds = strtoul(tokens[2], nullptr, 10);
    if (seconds == 0) {
      Serial.println("[GW] FAULT_ERROR duracao deve ser maior que zero");
      return;
    }
    durationMs = clampDuration(seconds * 1000ul);
  }
  probeSendCommand(static_cast<uint8_t>(node), kind, durationMs);
}

} // namespace

/* ===================================================================== */
/* Interface pública                                                     */
/* ===================================================================== */

void faultInjectionInit() {
  if (NODE_ID == 0) {
    Serial.println("[GW] Ensaios: FAULT POWER_CYCLE|SILENCE|AP_OUTAGE|DROP_SESSION|LOAD|CANCEL");
    return;
  }
  Serial.printf("[NODE %u] [ENSAIO] FAULT_INJECTION=%s\n", NODE_ID, IOT_FAULT_INJECTION ? "ENABLED" : "DISABLED");
}

bool faultInjectionSilent() {
#if IOT_FAULT_INJECTION
  return nodeFault == NodeFault::PowerCycle || nodeFault == NodeFault::Silence;
#else
  return false;
#endif
}

void faultInjectionPoll() {
  if (NODE_ID == 0) {
    probePollLoad();
    return;
  }
#if IOT_FAULT_INJECTION
  if (nodeFault != NodeFault::None && millis() - nodeFaultStartMs >= nodeFaultDurationMs) {
    endNodeFault(false);
  }
  if (normalModePending && nodeFault != NodeFault::PowerCycle && nodeFault != NodeFault::Silence &&
      millis() - lastNormalModeAttemptMs >= 100) {
    normalModePending = !requestNormalMode();
  }
#endif
}

bool faultInjectionHandleCanMessage(const CANMessage& rx) {
  if (rx.id == CAN_ID_TEST_LOAD_HIGH || rx.id == CAN_ID_TEST_LOAD_LOW) return true;

  if (rx.id == CAN_ID_TEST_CMD) {
#if IOT_FAULT_INJECTION
    if (NODE_ID != 0) handleNodeCommand(rx);
#endif
    return true;
  }

  if (rx.id == CAN_ID_TEST_STATUS) {
    if (NODE_ID == 0) probeHandleStatus(rx);
    return true;
  }

  return false;
}

bool faultInjectionHandleSerialCommand(const char* command) {
  if (NODE_ID != 0 || !command) return false;
  if (strncasecmp(command, "FAULT", 5) != 0 || (command[5] != ' ' && command[5] != '\0')) return false;

  static char line[96];
  strncpy(line, command + 5, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';

  char* tokens[5];
  uint8_t count = 0;
  char* save = nullptr;
  for (char* token = strtok_r(line, " \t", &save); token && count < 5;
       token = strtok_r(nullptr, " \t", &save)) {
    tokens[count++] = token;
  }
  probeHandleLine(tokens, count);
  return true;
}
