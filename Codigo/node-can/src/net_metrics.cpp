#include "net_metrics.h"

#include <string.h>
#include <strings.h>

#include "can_ids.h"
#include "node_config.h"

#include "ioc_canbits.h"

extern ACAN2515 can;

namespace {

constexpr uint8_t  MAX_IDS = 48;
constexpr uint32_t BUS_WINDOW_MS = 1000;
constexpr uint32_t PEAK_SLOT_MS = 100;       // resolução do pico de ocupação
constexpr uint32_t ID_WINDOW_MS = 5000;
constexpr uint32_t ID_LINE_GAP_MS = 40;      // no máximo uma linha ID_STATS por intervalo
constexpr uint32_t ID_IDLE_WINDOWS = 12;     // identificador sem tráfego por 60 s deixa a tabela

struct IdEntry {
  bool used = false;
  uint16_t id = 0;
  uint8_t dlc = 0;
  uint32_t count = 0;
  uint32_t bits = 0;
  bool haveLast = false;
  uint32_t lastUs = 0;
  uint32_t dtMinUs = 0;
  uint32_t dtMaxUs = 0;
  uint64_t dtSumUs = 0;
  uint32_t dtCount = 0;
  uint32_t windowStartMs = 0;
  uint8_t idleWindows = 0;
};

IdEntry ids[MAX_IDS];
bool enabled = true;

// Janela de 1 s.
uint32_t windowStartMs = 0;
uint32_t windowStartUs = 0;
uint32_t windowFrames = 0;
uint32_t windowBits = 0;
uint32_t windowOwnFrames = 0;
uint32_t slotStartMs = 0;
uint32_t slotBits = 0;
uint32_t peakSlotBits = 0;
uint32_t untrackedFrames = 0;   // quadros de identificadores que não couberam na tabela

// Contadores cumulativos.
uint32_t transfersOk = 0;
uint32_t transfersErr = 0;

uint8_t reportCursor = 0;
uint32_t lastIdLineMs = 0;

IdEntry* findOrCreate(uint16_t id, uint32_t nowMs) {
  IdEntry* freeSlot = nullptr;
  for (uint8_t i = 0; i < MAX_IDS; ++i) {
    if (ids[i].used && ids[i].id == id) return &ids[i];
    if (!ids[i].used && !freeSlot) freeSlot = &ids[i];
  }
  if (!freeSlot) return nullptr;
  *freeSlot = IdEntry{};
  freeSlot->used = true;
  freeSlot->id = id;
  freeSlot->windowStartMs = nowMs;
  return freeSlot;
}

void account(const CANMessage& frame, bool own) {
  if (!enabled || frame.ext || frame.rtr) return;
  const uint32_t nowMs = millis();
  const uint32_t nowUs = micros();
  const uint8_t dlc = frame.len > 8 ? 8 : frame.len;
  const uint16_t id = static_cast<uint16_t>(frame.id & 0x7FFu);
  const uint16_t bits = ioc_can_frame_bits(id, dlc, frame.data);

  if (nowMs - slotStartMs >= PEAK_SLOT_MS) {
    if (slotBits > peakSlotBits) peakSlotBits = slotBits;
    slotBits = 0;
    slotStartMs = nowMs;
  }
  slotBits += bits;
  windowBits += bits;
  ++windowFrames;
  if (own) ++windowOwnFrames;

  IdEntry* entry = findOrCreate(id, nowMs);
  if (!entry) {
    ++untrackedFrames;
    return;
  }
  if (entry->haveLast) {
    const uint32_t dt = nowUs - entry->lastUs;
    if (entry->dtCount == 0 || dt < entry->dtMinUs) entry->dtMinUs = dt;
    if (entry->dtCount == 0 || dt > entry->dtMaxUs) entry->dtMaxUs = dt;
    entry->dtSumUs += dt;
    ++entry->dtCount;
  }
  entry->haveLast = true;
  entry->lastUs = nowUs;
  entry->dlc = dlc;
  entry->bits += bits;
  ++entry->count;
  entry->idleWindows = 0;
}

uint8_t usedIds() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_IDS; ++i) if (ids[i].used) ++n;
  return n;
}

// Percentual com duas casas, sem ponto flutuante na formatação.
void formatPercent(char* out, size_t cap, uint64_t bits, uint64_t capacityBits) {
  const uint32_t hundredths = capacityBits ? static_cast<uint32_t>((bits * 10000ull) / capacityBits) : 0;
  snprintf(out, cap, "%lu.%02lu", static_cast<unsigned long>(hundredths / 100u),
           static_cast<unsigned long>(hundredths % 100u));
}

void emitBusStats(uint32_t nowMs, uint32_t nowUs) {
  if (slotBits > peakSlotBits) peakSlotBits = slotBits;
  const uint32_t elapsedUs = nowUs - windowStartUs;
  char load[12];
  char peak[12];
  formatPercent(load, sizeof(load), windowBits,
                (static_cast<uint64_t>(CAN_NOMINAL_BITRATE) * elapsedUs) / 1000000ull);
  formatPercent(peak, sizeof(peak), peakSlotBits,
                (static_cast<uint64_t>(CAN_NOMINAL_BITRATE) * PEAK_SLOT_MS) / 1000ull);

  Serial.printf("[GW] BUS_STATS t_ms=%lu win_ms=%lu frames=%lu bits=%lu load=%s peak=%s "
                "rx_err=%u tx_err=%u eflg=0x%02X rx_peak=%u ids=%u xfer_ok=%lu xfer_err=%lu "
                "own=%lu untracked=%lu\n",
                static_cast<unsigned long>(nowMs), static_cast<unsigned long>(nowMs - windowStartMs),
                static_cast<unsigned long>(windowFrames), static_cast<unsigned long>(windowBits),
                load, peak,
                static_cast<unsigned>(can.receiveErrorCounter()),
                static_cast<unsigned>(can.transmitErrorCounter()),
                static_cast<unsigned>(can.errorFlagRegister()),
                static_cast<unsigned>(can.receiveBufferPeakCount()),
                static_cast<unsigned>(usedIds()),
                static_cast<unsigned long>(transfersOk), static_cast<unsigned long>(transfersErr),
                static_cast<unsigned long>(windowOwnFrames), static_cast<unsigned long>(untrackedFrames));

  windowStartMs = nowMs;
  windowStartUs = nowUs;
  windowFrames = 0;
  windowBits = 0;
  windowOwnFrames = 0;
  peakSlotBits = 0;
  slotBits = 0;
  slotStartMs = nowMs;
}

// Emite no máximo uma linha por chamada, para não ocupar a serial em rajada.
void emitNextIdStats(uint32_t nowMs) {
  if (nowMs - lastIdLineMs < ID_LINE_GAP_MS) return;
  for (uint8_t step = 0; step < MAX_IDS; ++step) {
    IdEntry& entry = ids[reportCursor];
    reportCursor = static_cast<uint8_t>((reportCursor + 1u) % MAX_IDS);
    if (!entry.used || nowMs - entry.windowStartMs < ID_WINDOW_MS) continue;

    if (entry.count == 0) {
      if (++entry.idleWindows >= ID_IDLE_WINDOWS) entry.used = false;
      entry.windowStartMs = nowMs;
      continue;
    }

    const uint32_t avg = entry.dtCount ? static_cast<uint32_t>(entry.dtSumUs / entry.dtCount) : 0;
    Serial.printf("[GW] ID_STATS id=0x%03X win_ms=%lu n=%lu dlc=%u bits=%lu dt_min_us=%lu "
                  "dt_avg_us=%lu dt_max_us=%lu\n",
                  static_cast<unsigned>(entry.id), static_cast<unsigned long>(nowMs - entry.windowStartMs),
                  static_cast<unsigned long>(entry.count), static_cast<unsigned>(entry.dlc),
                  static_cast<unsigned long>(entry.bits), static_cast<unsigned long>(entry.dtMinUs),
                  static_cast<unsigned long>(avg), static_cast<unsigned long>(entry.dtMaxUs));
    entry.count = 0;
    entry.bits = 0;
    entry.dtCount = 0;
    entry.dtSumUs = 0;
    entry.dtMinUs = 0;
    entry.dtMaxUs = 0;
    entry.windowStartMs = nowMs;
    lastIdLineMs = nowMs;
    return;
  }
}

void resetAll() {
  const uint32_t nowMs = millis();
  for (uint8_t i = 0; i < MAX_IDS; ++i) ids[i] = IdEntry{};
  windowStartMs = nowMs;
  windowStartUs = micros();
  windowFrames = 0;
  windowBits = 0;
  windowOwnFrames = 0;
  slotStartMs = nowMs;
  slotBits = 0;
  peakSlotBits = 0;
  untrackedFrames = 0;
  transfersOk = 0;
  transfersErr = 0;
  reportCursor = 0;
  lastIdLineMs = nowMs;
}

} // namespace

void netMetricsInit() {
  if (NODE_ID != 0) return;
  resetAll();
  Serial.println("[GW] Metricas do barramento: METRICS ON|OFF|RESET");
}

void netMetricsOnReceive(const CANMessage& rx) {
  if (NODE_ID != 0) return;
  account(rx, false);
}

void netMetricsOnTransmit(const CANMessage& tx) {
  if (NODE_ID != 0) return;
  account(tx, true);
}

void netMetricsOnTransfer(bool ok) {
  if (NODE_ID != 0) return;
  if (ok) ++transfersOk; else ++transfersErr;
}

void netMetricsPoll() {
  if (NODE_ID != 0 || !enabled) return;
  const uint32_t nowMs = millis();
  if (nowMs - windowStartMs >= BUS_WINDOW_MS) emitBusStats(nowMs, micros());
  emitNextIdStats(nowMs);
}

bool netMetricsHandleSerialCommand(const char* command) {
  if (NODE_ID != 0 || !command) return false;
  if (strncasecmp(command, "METRICS", 7) != 0) return false;
  const char* arg = command + 7;
  while (*arg == ' ') ++arg;

  if (strcasecmp(arg, "ON") == 0) {
    if (!enabled) resetAll();
    enabled = true;
  } else if (strcasecmp(arg, "OFF") == 0) {
    enabled = false;
  } else if (strcasecmp(arg, "RESET") == 0) {
    resetAll();
  } else if (*arg != '\0') {
    Serial.println("[GW] METRICS_ERROR use: METRICS ON|OFF|RESET");
    return true;
  }
  Serial.printf("[GW] METRICS state=%s\n", enabled ? "ON" : "OFF");
  return true;
}
