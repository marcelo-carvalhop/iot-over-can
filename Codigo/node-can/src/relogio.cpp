#include "relogio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "can_ids.h"
#include "net_metrics.h"
#include "node_config.h"
#include "ioc_time.h"

extern bool canSend(const CANMessage& msg);   // wireless_link.cpp: envia e entrega aos painéis

namespace {

constexpr uint32_t BOOT_REQUEST_MS = 5000;     // pedido único depois da partida
constexpr uint32_t REQUEST_GAP_MS = 5000;      // intervalo mínimo entre pedidos
constexpr uint32_t REBASE_MS = 3600000;        // refaz a base bem antes da volta do millis()
constexpr uint32_t TOLERANCE_MS = 50;          // diferença que justifica corrigir a mesma fonte

uint8_t clockSource = IOC_TIME_SRC_NONE;
int64_t baseMs = 0;
uint32_t baseMillis = 0;
bool answers = false;
bool bootRequested = false;
bool requested = false;
uint32_t lastRequestMs = 0;

void setTime(int64_t utcMs, uint8_t source) {
  baseMs = utcMs;
  baseMillis = millis();
  clockSource = source;
}

void sendTime(uint8_t kind) {
  CANMessage msg;
  msg.id = CAN_ID_TIME_BASE + NODE_ID;
  msg.len = IOC_TIME_LEN;
  const ioc_time_msg t = {kind, clockSource, relogioValid() ? relogioNowMs() : 0};
  ioc_time_encode(&t, msg.data);
  if (canSend(msg)) netMetricsOnTransmit(msg);
}

void printTime(int64_t ms) {
  Serial.printf("epoch=%lu.%03u", static_cast<unsigned long>(ms / 1000), static_cast<unsigned>(ms % 1000));
}

// Linha de ajuste: na Probe 00 é lida pela TUI.
void logAdjust(uint8_t from, bool had, int64_t before, int64_t after) {
  if (NODE_ID == 0) {
    Serial.print("[GW] ");
  } else {
    Serial.printf("[NODE %u] [RELOGIO] ", static_cast<unsigned>(NODE_ID));
  }
  Serial.printf("TIME_SYNC node=%u fonte=%s ", static_cast<unsigned>(from), ioc_time_source_name(clockSource));
  printTime(after);
  if (had) {
    int64_t delta = after - before;
    if (delta > 2000000000LL) delta = 2000000000LL;
    if (delta < -2000000000LL) delta = -2000000000LL;
    Serial.printf(" ajuste_ms=%ld\n", static_cast<long>(delta));
  } else {
    Serial.println(" ajuste_ms=NA");
  }
}

}  // namespace

bool relogioValid() { return clockSource != IOC_TIME_SRC_NONE; }

int64_t relogioNowMs() { return baseMs + static_cast<int64_t>(static_cast<uint32_t>(millis() - baseMillis)); }

bool relogioOwnFrame(uint8_t* out) {
  if (!relogioValid()) return false;
  const ioc_time_msg t = {IOC_TIME_KIND_TIME, clockSource, relogioNowMs()};
  ioc_time_encode(&t, out);
  return true;
}

void relogioRequest() {
  const uint32_t now = millis();
  if (requested && now - lastRequestMs < REQUEST_GAP_MS) return;
  requested = true;
  lastRequestMs = now;
  sendTime(IOC_TIME_KIND_REQUEST);
}

void relogioPoll(bool answersRequests) {
  answers = answersRequests || NODE_ID == 0;
  const uint32_t now = millis();
  if (relogioValid() && now - baseMillis >= REBASE_MS) {
    baseMs += static_cast<int64_t>(now - baseMillis);
    baseMillis = now;
  }
  if (!bootRequested && NODE_ID != 0 && now >= BOOT_REQUEST_MS) {
    bootRequested = true;
    if (!relogioValid()) relogioRequest();
  }
}

bool relogioHandleCanMessage(const CANMessage& rx) {
  if (rx.ext || rx.rtr || rx.id < CAN_ID_TIME_BASE || rx.id > CAN_ID_TIME_LAST) return false;
  ioc_time_msg t;
  if (!ioc_time_decode(rx.data, rx.len, &t)) return true;
  const uint8_t from = static_cast<uint8_t>(rx.id - CAN_ID_TIME_BASE);
  if (t.kind == IOC_TIME_KIND_REQUEST) {
    if (NODE_ID == 0) Serial.printf("[GW] TIME_REQ node=%u\n", static_cast<unsigned>(from));
    if (answers && relogioValid() &&
        ioc_time_adopt(t.source, t.utc_ms, clockSource, relogioNowMs(), TOLERANCE_MS)) {
      sendTime(IOC_TIME_KIND_TIME);
    }
    return true;
  }
  const bool had = relogioValid();
  const int64_t before = relogioNowMs();
  if (!ioc_time_adopt(clockSource, before, t.source, t.utc_ms, TOLERANCE_MS)) return true;
  setTime(t.utc_ms, t.source);
  logAdjust(from, had, before, t.utc_ms);
  return true;
}

bool relogioHandleSerialCommand(const char* command) {
  if (strncasecmp(command, "TIME", 4) != 0 || (command[4] != ' ' && command[4] != '\0')) return false;
  const char* tag = NODE_ID == 0 ? "[GW]" : "[RELOGIO]";
  if (command[4] == '\0') {
    Serial.printf("%s TIME fonte=%s ", tag, ioc_time_source_name(clockSource));
    if (relogioValid()) {
      printTime(relogioNowMs());
      Serial.println();
    } else {
      Serial.println("epoch=NA");
    }
    return true;
  }
  char* end = nullptr;
  const long long ms = strtoll(command + 5, &end, 10);
  if (end == command + 5 || *end != '\0' || ms < static_cast<long long>(IOC_TIME_MIN_EPOCH) * 1000) {
    Serial.printf("%s TIME_ERROR uso: TIME <milissegundos UTC desde 1970>\n", tag);
    return true;
  }
  // O terminal é a melhor fonte: ajusta sempre e divulga a todos os Nodes.
  const bool had = relogioValid();
  const int64_t before = relogioNowMs();
  setTime(ms, IOC_TIME_SRC_TERMINAL);
  logAdjust(NODE_ID, had, before, ms);
  sendTime(IOC_TIME_KIND_TIME);
  return true;
}
