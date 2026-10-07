#include "wireless_discovery.h"

#include <NimBLEDevice.h>
#include <stdlib.h>
#include <strings.h>

#include "can_ids.h"
#include "node_config.h"
#include "protocolo.h"

extern ACAN2515 can;

namespace {

constexpr uint8_t BLE_COMPANY_LO = 0xFF;
constexpr uint8_t BLE_COMPANY_HI = 0xFF;
constexpr uint8_t BLE_MAGIC_0 = 0x49; // 'I'
constexpr uint8_t BLE_MAGIC_1 = 0x43; // 'C'
constexpr uint8_t BLE_ADV_VERSION = 0x01;
constexpr uint8_t BLE_MANUFACTURER_LEN = 15;
constexpr uint8_t MAX_CANDIDATES = 8;
constexpr uint8_t MAX_BINDINGS = 4;
constexpr int RSSI_REPORT_DELTA_DB = 4;
constexpr uint32_t REPORT_REFRESH_MS = 5000;
constexpr uint32_t BIND_CANDIDATE_MAX_AGE_MS = 7000;
constexpr uint32_t ASSOCIATING_MIN_MS = 250;
constexpr uint32_t ONLINE_STALE_MS = 15000;
constexpr uint32_t STALE_LOST_MS = 45000;
constexpr uint32_t SCAN_WATCHDOG_MS = 2000;

struct Candidate {
  bool used = false;
  uint64_t uuid = 0;
  uint8_t profile = 0;
  uint8_t protocol = 0;
  int8_t rssi = -127;
  uint32_t lastSeenMs = 0;
  uint32_t lastReportMs = 0;
  bool pending = false;
};

struct DiscoveryAssembly {
  bool havePartA = false;
  uint8_t seq = 0;
  uint8_t uuidFirst7[7] = {0};
};

struct AssocCommandAssembly {
  bool havePartA = false;
  uint8_t seq = 0;
  uint8_t target = 0;
  uint8_t action = 0;
  uint8_t uuidFirst5[5] = {0};
};

struct AssocStatusAssembly {
  bool havePartA = false;
  uint8_t seq = 0;
  uint8_t childId = 0;
  uint8_t state = WIRELESS_ASSOC_DISCOVERED;
  uint8_t profile = 0;
  uint8_t uuidFirst4[4] = {0};
};

struct Binding {
  bool used = false;
  uint64_t uuid = 0;
  uint8_t childId = 0;
  uint8_t profile = 0;
  uint8_t protocol = 0;
  int8_t rssi = -127;
  uint8_t assocState = WIRELESS_ASSOC_DISCOVERED;
  uint32_t lastSeenMs = 0;
  uint32_t stateSinceMs = 0;
};

Candidate candidates[MAX_CANDIDATES];
DiscoveryAssembly discoveryAssemblies[32];
AssocStatusAssembly assocStatusAssemblies[32];
AssocCommandAssembly assocCommandAssembly;
Binding bindings[MAX_BINDINGS];
portMUX_TYPE candidateMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t reportSeq = 0;
uint8_t assocCommandSeq = 0;
uint8_t assocStatusSeq = 0;
bool scannerStarted = false;
uint32_t lastScanWatchdogMs = 0;

const char* profileName(uint8_t profile) {
  switch (profile) {
    case 0x01: return "VIBRATION";
    case 0x02: return "ACOUSTIC";
    case 0x03: return "THERMAL";
    default: return "UNKNOWN";
  }
}

const char* assocStateName(uint8_t state) {
  switch (state) {
    case WIRELESS_ASSOC_DISCOVERED: return "DISCOVERED";
    case WIRELESS_ASSOC_ASSOCIATING: return "ASSOCIATING";
    case WIRELESS_ASSOC_BOUND: return "BOUND";
    case WIRELESS_ASSOC_ONLINE: return "ONLINE";
    case WIRELESS_ASSOC_STALE: return "STALE";
    case WIRELESS_ASSOC_LOST: return "LOST";
    case WIRELESS_ASSOC_UNBOUND: return "UNBOUND";
    case WIRELESS_ASSOC_REJECTED: return "REJECTED";
    default: return "UNKNOWN";
  }
}

int findCandidate(uint64_t uuid) {
  for (uint8_t i = 0; i < MAX_CANDIDATES; ++i) {
    if (candidates[i].used && candidates[i].uuid == uuid) return i;
  }
  return -1;
}

int allocateCandidate() {
  for (uint8_t i = 0; i < MAX_CANDIDATES; ++i) {
    if (!candidates[i].used) return i;
  }
  return 0; // cache limitada: substitui o slot 0 quando cheia
}

int findBindingByUuid(uint64_t uuid) {
  for (uint8_t i = 0; i < MAX_BINDINGS; ++i) {
    if (bindings[i].used && bindings[i].uuid == uuid) return i;
  }
  return -1;
}

int allocateBinding() {
  for (uint8_t i = 0; i < MAX_BINDINGS; ++i) {
    if (!bindings[i].used) return i;
  }
  return -1;
}

uint8_t allocateChildId() {
  for (uint8_t candidate = 1; candidate <= MAX_BINDINGS; ++candidate) {
    bool used = false;
    for (uint8_t i = 0; i < MAX_BINDINGS; ++i) {
      if (bindings[i].used && bindings[i].childId == candidate) {
        used = true;
        break;
      }
    }
    if (!used) return candidate;
  }
  return 0;
}

void sendAssociationStatus(const Binding& binding, uint8_t stateOverride = 0) {
  if (NODE_ID == 0) return;

  const uint16_t baseId = CAN_ID_WIRELESS_ASSOC_STATUS_BASE + static_cast<uint16_t>(NODE_ID) * 2u;
  const uint8_t seq = ++assocStatusSeq;
  const uint8_t state = stateOverride ? stateOverride : binding.assocState;

  CANMessage a;
  a.id = baseId;
  a.len = 8;
  a.data[0] = seq;
  a.data[1] = binding.childId;
  a.data[2] = state;
  a.data[3] = binding.profile;
  for (uint8_t i = 0; i < 4; ++i) {
    a.data[4 + i] = static_cast<uint8_t>((binding.uuid >> (56 - 8 * i)) & 0xFFu);
  }

  CANMessage b;
  b.id = baseId + 1u;
  b.len = 8;
  b.data[0] = seq;
  b.data[1] = binding.childId;
  b.data[2] = static_cast<uint8_t>(binding.rssi);
  b.data[3] = binding.protocol;
  for (uint8_t i = 0; i < 4; ++i) {
    b.data[4 + i] = static_cast<uint8_t>((binding.uuid >> (24 - 8 * i)) & 0xFFu);
  }

  can.tryToSend(a);
  can.tryToSend(b);

  Serial.printf("[NODE %u] [WIRELESS ASSOC] child=%u uuid=0x%016llX state=%s rssi=%d\n",
                NODE_ID,
                static_cast<unsigned>(binding.childId),
                static_cast<unsigned long long>(binding.uuid),
                assocStateName(state),
                static_cast<int>(binding.rssi));
}

void sendRejectedAssociation(uint64_t uuid, uint8_t profile, uint8_t protocol, int8_t rssi) {
  Binding rejected;
  rejected.used = true;
  rejected.uuid = uuid;
  rejected.childId = 0;
  rejected.profile = profile;
  rejected.protocol = protocol;
  rejected.rssi = rssi;
  rejected.assocState = WIRELESS_ASSOC_REJECTED;
  sendAssociationStatus(rejected, WIRELESS_ASSOC_REJECTED);
}

void processBindRequest(uint64_t uuid) {
  if (NODE_ID == 0) return;
  const uint32_t now = millis();

  portENTER_CRITICAL(&candidateMux);
  int existingIndex = findBindingByUuid(uuid);
  if (existingIndex >= 0) {
    Binding snapshot = bindings[existingIndex];
    portEXIT_CRITICAL(&candidateMux);
    sendAssociationStatus(snapshot);
    return;
  }

  const int candidateIndex = findCandidate(uuid);
  if (candidateIndex < 0 || now - candidates[candidateIndex].lastSeenMs > BIND_CANDIDATE_MAX_AGE_MS) {
    portEXIT_CRITICAL(&candidateMux);
    sendRejectedAssociation(uuid, 0, 0, -127);
    return;
  }

  const int bindingIndex = allocateBinding();
  const uint8_t childId = allocateChildId();
  if (bindingIndex < 0 || childId == 0) {
    Candidate c = candidates[candidateIndex];
    portEXIT_CRITICAL(&candidateMux);
    sendRejectedAssociation(uuid, c.profile, c.protocol, c.rssi);
    return;
  }

  Candidate c = candidates[candidateIndex];
  Binding& binding = bindings[bindingIndex];
  binding = Binding{};
  binding.used = true;
  binding.uuid = uuid;
  binding.childId = childId;
  binding.profile = c.profile;
  binding.protocol = c.protocol;
  binding.rssi = c.rssi;
  binding.assocState = WIRELESS_ASSOC_ASSOCIATING;
  binding.lastSeenMs = c.lastSeenMs;
  binding.stateSinceMs = now;
  Binding snapshot = binding;
  portEXIT_CRITICAL(&candidateMux);

  sendAssociationStatus(snapshot);
}

void processUnbindRequest(uint64_t uuid) {
  if (NODE_ID == 0) return;

  portENTER_CRITICAL(&candidateMux);
  const int bindingIndex = findBindingByUuid(uuid);
  if (bindingIndex < 0) {
    portEXIT_CRITICAL(&candidateMux);
    sendRejectedAssociation(uuid, 0, 0, -127);
    return;
  }

  Binding snapshot = bindings[bindingIndex];
  snapshot.assocState = WIRELESS_ASSOC_UNBOUND;
  bindings[bindingIndex] = Binding{};
  portEXIT_CRITICAL(&candidateMux);

  sendAssociationStatus(snapshot, WIRELESS_ASSOC_UNBOUND);
}

void processAssociationCommand(uint8_t action, uint64_t uuid) {
  if (action == WIRELESS_ASSOC_ACTION_BIND) {
    processBindRequest(uuid);
  } else if (action == WIRELESS_ASSOC_ACTION_UNBIND) {
    processUnbindRequest(uuid);
  }
}

class DiscoveryScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    if (!advertisedDevice || !advertisedDevice->haveManufacturerData()) return;
    std::string md = advertisedDevice->getManufacturerData();
    if (md.size() < BLE_MANUFACTURER_LEN) return;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(md.data());
    if (d[0] != BLE_COMPANY_LO || d[1] != BLE_COMPANY_HI ||
        d[2] != BLE_MAGIC_0 || d[3] != BLE_MAGIC_1 ||
        d[4] != BLE_ADV_VERSION) return;

    uint64_t uuid = 0;
    for (uint8_t i = 7; i < 15; ++i) uuid = (uuid << 8) | d[i];
    if (uuid == 0) return;

    const uint8_t profile = d[5];
    const uint8_t protocol = d[6];
    int rssiValue = advertisedDevice->getRSSI();
    if (rssiValue < -127) rssiValue = -127;
    if (rssiValue > 20) rssiValue = 20;
    const int8_t rssi = static_cast<int8_t>(rssiValue);
    const uint32_t now = millis();

    portENTER_CRITICAL(&candidateMux);
    int idx = findCandidate(uuid);
    if (idx < 0) {
      idx = allocateCandidate();
      candidates[idx] = Candidate{};
      candidates[idx].used = true;
      candidates[idx].uuid = uuid;
      candidates[idx].pending = true;
    }
    Candidate& c = candidates[idx];
    const int diff = abs(static_cast<int>(rssi) - static_cast<int>(c.rssi));
    if (c.lastReportMs == 0 || diff >= RSSI_REPORT_DELTA_DB || now - c.lastReportMs >= REPORT_REFRESH_MS) {
      c.pending = true;
    }
    c.profile = profile;
    c.protocol = protocol;
    c.rssi = rssi;
    c.lastSeenMs = now;

    const int bindingIndex = findBindingByUuid(uuid);
    if (bindingIndex >= 0) {
      Binding& binding = bindings[bindingIndex];
      binding.profile = profile;
      binding.protocol = protocol;
      binding.rssi = rssi;
      binding.lastSeenMs = now;
    }
    portEXIT_CRITICAL(&candidateMux);
  }
};

DiscoveryScanCallbacks scanCallbacks;

void sendCandidateReport(const Candidate& c) {
  const uint16_t baseId = CAN_ID_WIRELESS_DISCOVERY_BASE + static_cast<uint16_t>(NODE_ID) * 2u;
  const uint8_t seq = ++reportSeq;

  CANMessage a;
  a.id = baseId;
  a.len = 8;
  a.data[0] = seq;
  for (uint8_t i = 0; i < 7; ++i) {
    a.data[1 + i] = static_cast<uint8_t>((c.uuid >> (56 - 8 * i)) & 0xFFu);
  }

  CANMessage b;
  b.id = baseId + 1u;
  b.len = 8;
  b.data[0] = seq;
  b.data[1] = static_cast<uint8_t>(c.uuid & 0xFFu);
  b.data[2] = c.profile;
  b.data[3] = static_cast<uint8_t>(c.rssi);
  b.data[4] = c.protocol;
  b.data[5] = BLE_ADV_VERSION;
  b.data[6] = 0x00;
  b.data[7] = 0x00;

  can.tryToSend(a);
  can.tryToSend(b);

  Serial.printf("[NODE %u] [BLE] candidate uuid=0x%016llX profile=%s rssi=%d protocol=%u\n",
                NODE_ID, static_cast<unsigned long long>(c.uuid), profileName(c.profile),
                static_cast<int>(c.rssi), static_cast<unsigned>(c.protocol));
}

void printProbeCandidate(uint8_t reporter, const DiscoveryAssembly& as, const CANMessage& b) {
  uint64_t uuid = 0;
  for (uint8_t i = 0; i < 7; ++i) uuid = (uuid << 8) | as.uuidFirst7[i];
  uuid = (uuid << 8) | b.data[1];
  const uint8_t profile = b.data[2];
  const int8_t rssi = static_cast<int8_t>(b.data[3]);
  const uint8_t protocol = b.data[4];

  Serial.printf("[GW] WIRELESS_CANDIDATE reporter=%u uuid=0x%016llX profile=%s rssi=%d protocol=%u\n",
                reporter, static_cast<unsigned long long>(uuid), profileName(profile),
                static_cast<int>(rssi), static_cast<unsigned>(protocol));
}

void printProbeAssociation(uint8_t reporter, const AssocStatusAssembly& as, const CANMessage& b) {
  uint64_t uuid = 0;
  for (uint8_t i = 0; i < 4; ++i) uuid = (uuid << 8) | as.uuidFirst4[i];
  for (uint8_t i = 0; i < 4; ++i) uuid = (uuid << 8) | b.data[4 + i];
  const int8_t rssi = static_cast<int8_t>(b.data[2]);
  const uint8_t protocol = b.data[3];

  Serial.printf("[GW] WIRELESS_ASSOC node=%u child=%u uuid=0x%016llX profile=%s state=%s rssi=%d protocol=%u\n",
                reporter,
                static_cast<unsigned>(as.childId),
                static_cast<unsigned long long>(uuid),
                profileName(as.profile),
                assocStateName(as.state),
                static_cast<int>(rssi),
                static_cast<unsigned>(protocol));
}

bool parseUuid(const char* text, uint64_t& uuid) {
  if (!text || !*text) return false;
  char* end = nullptr;
  uuid = strtoull(text, &end, 0);
  return end && *end == '\0' && uuid != 0;
}

void sendAssociationCommand(uint8_t target, uint8_t action, uint64_t uuid) {
  const uint8_t seq = ++assocCommandSeq;

  CANMessage a;
  a.id = CAN_ID_WIRELESS_ASSOC_CMD_A;
  a.len = 8;
  a.data[0] = seq;
  a.data[1] = target;
  a.data[2] = action;
  for (uint8_t i = 0; i < 5; ++i) {
    a.data[3 + i] = static_cast<uint8_t>((uuid >> (56 - 8 * i)) & 0xFFu);
  }

  CANMessage b;
  b.id = CAN_ID_WIRELESS_ASSOC_CMD_B;
  b.len = 8;
  b.data[0] = seq;
  b.data[1] = target;
  b.data[2] = action;
  for (uint8_t i = 0; i < 3; ++i) {
    b.data[3 + i] = static_cast<uint8_t>((uuid >> (16 - 8 * i)) & 0xFFu);
  }
  b.data[6] = 0;
  b.data[7] = 0;

  can.tryToSend(a);
  can.tryToSend(b);

  Serial.printf("[GW] WIRELESS_%s_TX node=%u uuid=0x%016llX seq=%u\n",
                action == WIRELESS_ASSOC_ACTION_BIND ? "BIND" : "UNBIND",
                static_cast<unsigned>(target),
                static_cast<unsigned long long>(uuid),
                static_cast<unsigned>(seq));
}

void pollBindings() {
  if (NODE_ID == 0) return;
  const uint32_t now = millis();

  for (uint8_t i = 0; i < MAX_BINDINGS; ++i) {
    Binding snapshot;
    bool publish = false;

    portENTER_CRITICAL(&candidateMux);
    Binding& binding = bindings[i];
    if (!binding.used) {
      portEXIT_CRITICAL(&candidateMux);
      continue;
    }

    const uint32_t age = now - binding.lastSeenMs;
    if (binding.assocState == WIRELESS_ASSOC_ASSOCIATING &&
        now - binding.stateSinceMs >= ASSOCIATING_MIN_MS &&
        age <= BIND_CANDIDATE_MAX_AGE_MS) {
      binding.assocState = WIRELESS_ASSOC_BOUND;
      binding.stateSinceMs = now;
      snapshot = binding;
      publish = true;
    } else if (binding.assocState == WIRELESS_ASSOC_BOUND &&
               binding.lastSeenMs > binding.stateSinceMs) {
      binding.assocState = WIRELESS_ASSOC_ONLINE;
      binding.stateSinceMs = now;
      snapshot = binding;
      publish = true;
    } else if (binding.assocState == WIRELESS_ASSOC_BOUND && age > ONLINE_STALE_MS) {
      binding.assocState = WIRELESS_ASSOC_STALE;
      binding.stateSinceMs = now;
      snapshot = binding;
      publish = true;
    } else if (binding.assocState == WIRELESS_ASSOC_ONLINE && age > ONLINE_STALE_MS) {
      binding.assocState = WIRELESS_ASSOC_STALE;
      binding.stateSinceMs = now;
      snapshot = binding;
      publish = true;
    } else if (binding.assocState == WIRELESS_ASSOC_STALE && age <= ONLINE_STALE_MS) {
      binding.assocState = WIRELESS_ASSOC_ONLINE;
      binding.stateSinceMs = now;
      snapshot = binding;
      publish = true;
    } else if (binding.assocState == WIRELESS_ASSOC_STALE && age > STALE_LOST_MS) {
      binding.assocState = WIRELESS_ASSOC_LOST;
      binding.stateSinceMs = now;
      snapshot = binding;
      publish = true;
    } else if (binding.assocState == WIRELESS_ASSOC_LOST && age <= ONLINE_STALE_MS) {
      binding.assocState = WIRELESS_ASSOC_ONLINE;
      binding.stateSinceMs = now;
      snapshot = binding;
      publish = true;
    }
    portEXIT_CRITICAL(&candidateMux);

    if (publish) sendAssociationStatus(snapshot);
  }
}

} // namespace

void wirelessDiscoveryInit() {
  if (NODE_ID == 0) {
    Serial.println("[GW] BLE scanner disabled on Probe 00");
    return;
  }

  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);
  scan->setDuplicateFilter(0);
  scan->setActiveScan(false);
  scan->setInterval(160);
  scan->setWindow(80);
  scan->setMaxResults(0);
  scannerStarted = scan->start(0, false, true);
  Serial.printf("[NODE %u] BLE_SCAN=%s\n", NODE_ID, scannerStarted ? "ACTIVE" : "ERROR");
}

void wirelessDiscoveryPoll() {
  if (NODE_ID != 0) {
    const uint32_t now = millis();
    NimBLEScan* scan = NimBLEDevice::getScan();

    if (now - lastScanWatchdogMs >= SCAN_WATCHDOG_MS) {
      lastScanWatchdogMs = now;
      if (!scan->isScanning()) {
        scannerStarted = scan->start(0, false, true);
        Serial.printf("[NODE %u] BLE_SCAN_RESTART=%s\n",
                      NODE_ID, scannerStarted ? "ACTIVE" : "ERROR");
      } else {
        scannerStarted = true;
      }
    }

    if (scannerStarted) {
      Candidate ready;
      bool haveReady = false;

      portENTER_CRITICAL(&candidateMux);
      for (uint8_t i = 0; i < MAX_CANDIDATES; ++i) {
        if (candidates[i].used && candidates[i].pending) {
          ready = candidates[i];
          candidates[i].pending = false;
          candidates[i].lastReportMs = now;
          haveReady = true;
          break;
        }
      }
      portEXIT_CRITICAL(&candidateMux);

      if (haveReady) sendCandidateReport(ready);
    }
  }

  pollBindings();
}

bool wirelessAssociationHandleSerialCommand(const char* command) {
  if (NODE_ID != 0 || !command) return false;

  char verb[16] = {0};
  unsigned int target = 0;
  char uuidText[32] = {0};
  if (sscanf(command, "WIRELESS %15s %u %31s", verb, &target, uuidText) != 3) {
    return false;
  }
  if (target == 0 || target >= 32) {
    Serial.println("[GW] WIRELESS_ERROR target invalido; use Node 1..31");
    return true;
  }

  uint64_t uuid = 0;
  if (!parseUuid(uuidText, uuid)) {
    Serial.println("[GW] WIRELESS_ERROR UUID invalido");
    return true;
  }

  if (strcasecmp(verb, "BIND") == 0) {
    sendAssociationCommand(static_cast<uint8_t>(target), WIRELESS_ASSOC_ACTION_BIND, uuid);
    return true;
  }
  if (strcasecmp(verb, "UNBIND") == 0) {
    sendAssociationCommand(static_cast<uint8_t>(target), WIRELESS_ASSOC_ACTION_UNBIND, uuid);
    return true;
  }

  Serial.println("[GW] WIRELESS_ERROR use: WIRELESS BIND|UNBIND <node> <uuid>");
  return true;
}

bool wirelessDiscoveryHandleCanMessage(const CANMessage& rx) {
  if (rx.id >= CAN_ID_WIRELESS_DISCOVERY_BASE && rx.id <= CAN_ID_WIRELESS_DISCOVERY_LAST) {
    if (rx.len != 8) return true;

    const uint16_t offset = static_cast<uint16_t>(rx.id - CAN_ID_WIRELESS_DISCOVERY_BASE);
    const uint8_t reporter = static_cast<uint8_t>(offset / 2u);
    const uint8_t part = static_cast<uint8_t>(offset & 1u);
    if (reporter == 0 || reporter >= 32) return true;

    DiscoveryAssembly& as = discoveryAssemblies[reporter];
    if (part == 0) {
      as.havePartA = true;
      as.seq = rx.data[0];
      memcpy(as.uuidFirst7, &rx.data[1], 7);
      return true;
    }

    if (NODE_ID == 0 && as.havePartA && as.seq == rx.data[0]) {
      printProbeCandidate(reporter, as, rx);
    }
    as.havePartA = false;
    return true;
  }

  if (rx.id == CAN_ID_WIRELESS_ASSOC_CMD_A) {
    if (rx.len != 8 || NODE_ID == 0) return true;
    if (rx.data[1] != NODE_ID) return true;
    assocCommandAssembly.havePartA = true;
    assocCommandAssembly.seq = rx.data[0];
    assocCommandAssembly.target = rx.data[1];
    assocCommandAssembly.action = rx.data[2];
    memcpy(assocCommandAssembly.uuidFirst5, &rx.data[3], 5);
    return true;
  }

  if (rx.id == CAN_ID_WIRELESS_ASSOC_CMD_B) {
    if (rx.len != 8 || NODE_ID == 0) return true;
    if (!assocCommandAssembly.havePartA ||
        assocCommandAssembly.seq != rx.data[0] ||
        assocCommandAssembly.target != rx.data[1] ||
        assocCommandAssembly.action != rx.data[2]) {
      assocCommandAssembly.havePartA = false;
      return true;
    }

    uint64_t uuid = 0;
    for (uint8_t i = 0; i < 5; ++i) uuid = (uuid << 8) | assocCommandAssembly.uuidFirst5[i];
    for (uint8_t i = 0; i < 3; ++i) uuid = (uuid << 8) | rx.data[3 + i];
    const uint8_t action = assocCommandAssembly.action;
    assocCommandAssembly.havePartA = false;
    processAssociationCommand(action, uuid);
    return true;
  }

  if (rx.id >= CAN_ID_WIRELESS_ASSOC_STATUS_BASE && rx.id <= CAN_ID_WIRELESS_ASSOC_STATUS_LAST) {
    if (rx.len != 8) return true;
    const uint16_t offset = static_cast<uint16_t>(rx.id - CAN_ID_WIRELESS_ASSOC_STATUS_BASE);
    const uint8_t reporter = static_cast<uint8_t>(offset / 2u);
    const uint8_t part = static_cast<uint8_t>(offset & 1u);
    if (reporter == 0 || reporter >= 32) return true;

    AssocStatusAssembly& as = assocStatusAssemblies[reporter];
    if (part == 0) {
      as.havePartA = true;
      as.seq = rx.data[0];
      as.childId = rx.data[1];
      as.state = rx.data[2];
      as.profile = rx.data[3];
      memcpy(as.uuidFirst4, &rx.data[4], 4);
      return true;
    }

    if (NODE_ID == 0 && as.havePartA && as.seq == rx.data[0] && as.childId == rx.data[1]) {
      printProbeAssociation(reporter, as, rx);
    }
    as.havePartA = false;
    return true;
  }

  return false;
}
