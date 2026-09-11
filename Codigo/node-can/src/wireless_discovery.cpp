\
#include "wireless_discovery.h"

#include <NimBLEDevice.h>

#include "can_ids.h"
#include "node_config.h"

extern ACAN2515 can;

namespace {

constexpr uint8_t BLE_COMPANY_LO = 0xFF;
constexpr uint8_t BLE_COMPANY_HI = 0xFF;
constexpr uint8_t BLE_MAGIC_0 = 0x49; // 'I'
constexpr uint8_t BLE_MAGIC_1 = 0x43; // 'C'
constexpr uint8_t BLE_ADV_VERSION = 0x01;
constexpr uint8_t BLE_MANUFACTURER_LEN = 15;
constexpr uint8_t MAX_CANDIDATES = 8;
constexpr int RSSI_REPORT_DELTA_DB = 4;
constexpr uint32_t REPORT_REFRESH_MS = 5000;

struct Candidate {
  bool used = false;
  uint64_t uuid = 0;
  uint8_t profile = 0;
  uint8_t protocol = 0;
  int8_t rssi = -127;
  uint32_t lastReportMs = 0;
  bool pending = false;
};

struct Assembly {
  bool havePartA = false;
  uint8_t seq = 0;
  uint8_t uuidFirst7[7] = {0};
};

Candidate candidates[MAX_CANDIDATES];
Assembly assemblies[32];
portMUX_TYPE candidateMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t reportSeq = 0;
bool scannerStarted = false;

const char* profileName(uint8_t profile) {
  switch (profile) {
    case 0x01: return "VIBRATION";
    case 0x02: return "ACOUSTIC";
    case 0x03: return "THERMAL";
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
  return 0; // bounded cache: replace oldest slot 0 in this first baseline
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

void printGatewayCandidate(uint8_t reporter, const Assembly& as, const CANMessage& b) {
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

} // namespace

void wirelessDiscoveryInit() {
  if (NODE_ID == 0) {
    Serial.println("[GW] BLE scanner disabled on Probe 00");
    return;
  }

  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);
  scan->setActiveScan(false);
  scan->setInterval(160);
  scan->setWindow(80);
  scan->setMaxResults(0);
  scannerStarted = scan->start(0, false, true);
  Serial.printf("[NODE %u] BLE_SCAN=%s\n", NODE_ID, scannerStarted ? "ACTIVE" : "ERROR");
}

void wirelessDiscoveryPoll() {
  if (NODE_ID == 0 || !scannerStarted) return;

  Candidate ready;
  bool haveReady = false;
  const uint32_t now = millis();

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

bool wirelessDiscoveryHandleCanMessage(const CANMessage& rx) {
  if (rx.id < CAN_ID_WIRELESS_DISCOVERY_BASE || rx.id > CAN_ID_WIRELESS_DISCOVERY_LAST) return false;
  if (rx.len != 8) return true;

  const uint16_t offset = static_cast<uint16_t>(rx.id - CAN_ID_WIRELESS_DISCOVERY_BASE);
  const uint8_t reporter = static_cast<uint8_t>(offset / 2u);
  const uint8_t part = static_cast<uint8_t>(offset & 1u);
  if (reporter == 0 || reporter >= 32) return true;

  Assembly& as = assemblies[reporter];
  if (part == 0) {
    as.havePartA = true;
    as.seq = rx.data[0];
    memcpy(as.uuidFirst7, &rx.data[1], 7);
    return true;
  }

  if (NODE_ID == 0 && as.havePartA && as.seq == rx.data[0]) {
    printGatewayCandidate(reporter, as, rx);
  }
  as.havePartA = false;
  return true;
}
