#include "wireless_discovery.h"

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <stdlib.h>
#include <strings.h>

#include "can_ids.h"
#include "net_metrics.h"
#include "node_config.h"
#include "node_types.h"
#include "protocolo.h"
#include "wireless_link.h"

#include "ioc_bindstore.h"
#include "ioc_failover.h"

extern ACAN2515 can;
extern NodeState state;

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
// Uma reassociação decidida pelo líder parte de uma observação que já
// atravessou o barramento; a tolerância cobre o intervalo entre relatórios.
constexpr uint32_t FAILOVER_CANDIDATE_MAX_AGE_MS = 15000;
constexpr uint32_t FAILOVER_POLL_MS = 1000;
constexpr uint32_t ASSOCIATING_MIN_MS = 250;
constexpr uint32_t ONLINE_STALE_MS = 15000;
constexpr uint32_t STALE_LOST_MS = 45000;
constexpr uint32_t SCAN_WATCHDOG_MS = 2000;
// O estado do vínculo é republicado periodicamente para que uma TUI aberta
// depois da associação reconstrua a topologia sem depender de uma transição.
constexpr uint32_t ASSOC_REFRESH_MS = 10000;
// Vínculos gravados na NVS só são retomados depois de o Node ouvir a rede por
// mais que um período de republicação: se outro Node assumiu o sensor durante
// a ausência, o vínculo gravado é descartado em vez de disputado.
constexpr uint32_t RESTORE_GRACE_MS = ASSOC_REFRESH_MS + 2000;
constexpr const char* NVS_NAMESPACE = "ioc-bind";
constexpr const char* NVS_KEY = "tbl";

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

// Dados de uma reassociação automática, levados nos dois últimos bytes do
// segundo quadro do comando (zero nos comandos do operador).
struct FailoverInfo {
  uint8_t fromNode = 0;
  int8_t rssi = 0;
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
  uint32_t lastPublishMs = 0;
  bool autoFailover = false;    // o operador autorizou a reassociação automática
  bool publishedSecure = false; // situação da sessão de dados no último estado publicado
  bool restored = false;        // retomado da NVS; cede em disputa até reabrir a sessão
};

Candidate candidates[MAX_CANDIDATES];
DiscoveryAssembly discoveryAssemblies[32];
AssocStatusAssembly assocStatusAssemblies[32];
AssocCommandAssembly assocCommandAssembly;    // pedidos da Probe 00 (0x300/0x301)
AssocCommandAssembly leaderCommandAssembly;   // pedidos do líder (0x302/0x303)
Binding bindings[MAX_BINDINGS];
portMUX_TYPE candidateMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t reportSeq = 0;
uint8_t assocCommandSeq = 0;
uint8_t assocStatusSeq = 0;
bool scannerStarted = false;
uint32_t lastScanWatchdogMs = 0;
// Visão da rede usada na reassociação automática. Todos os Nodes a mantêm;
// apenas o líder decide (ver ioc_failover.h).
ioc_fo_table failoverTable;
uint32_t lastFailoverPollMs = 0;
// Vínculos lidos da NVS no boot, à espera do fim de RESTORE_GRACE_MS. Usados
// só no laço principal (tratamento do CAN e wirelessDiscoveryPoll).
ioc_bs_entry pendingRestore[MAX_BINDINGS];
uint8_t pendingRestoreCount = 0;
uint32_t restoreDeadlineMs = 0;
bool restoreLoaded = false;

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

bool childIdInUse(uint8_t childId) {
  for (uint8_t i = 0; i < MAX_BINDINGS; ++i) {
    if (bindings[i].used && bindings[i].childId == childId) return true;
  }
  // Um vínculo gravado ainda por retomar mantém o seu número de filho.
  for (uint8_t i = 0; i < pendingRestoreCount; ++i) {
    if (pendingRestore[i].child_id == childId) return true;
  }
  return false;
}

uint8_t allocateChildId() {
  for (uint8_t candidate = 1; candidate <= MAX_BINDINGS; ++candidate) {
    if (!childIdInUse(candidate)) return candidate;
  }
  return 0;
}

int findPendingRestore(uint64_t uuid) {
  for (uint8_t i = 0; i < pendingRestoreCount; ++i) {
    if (pendingRestore[i].uuid == uuid) return i;
  }
  return -1;
}

ioc_bs_entry takePendingRestore(int index) {
  const ioc_bs_entry entry = pendingRestore[index];
  pendingRestore[index] = pendingRestore[--pendingRestoreCount];
  return entry;
}

// Grava a tabela de vínculos (inclusive os ainda por retomar) na NVS. Fora de
// seção crítica: a escrita na flash não pode acontecer com interrupções
// desligadas. Os vínculos mudam por ação do operador ou do líder, raramente,
// e o desgaste da flash é desprezível.
void saveBindings() {
  if (NODE_ID == 0) return;
  ioc_bs_entry entries[MAX_BINDINGS + MAX_BINDINGS];
  uint8_t count = 0;
  portENTER_CRITICAL(&candidateMux);
  for (uint8_t i = 0; i < MAX_BINDINGS; ++i) {
    if (!bindings[i].used) continue;
    entries[count].uuid = bindings[i].uuid;
    entries[count].child_id = bindings[i].childId;
    entries[count].profile = bindings[i].profile;
    entries[count].protocol = bindings[i].protocol;
    entries[count].flags = bindings[i].autoFailover ? IOC_BS_FLAG_AUTO_FAILOVER : 0;
    ++count;
  }
  portEXIT_CRITICAL(&candidateMux);
  for (uint8_t i = 0; i < pendingRestoreCount && count < MAX_BINDINGS; ++i) entries[count++] = pendingRestore[i];

  uint8_t blob[IOC_BS_MAX_BYTES];
  const size_t len = ioc_bs_encode(NODE_ID, entries, count, blob, sizeof(blob));
  bool ok = false;
  Preferences prefs;
  if (len > 0 && prefs.begin(NVS_NAMESPACE, false)) {
    ok = prefs.putBytes(NVS_KEY, blob, len) == len;
    prefs.end();
  }
  Serial.printf("[NODE %u] [WIRELESS ASSOC] tabela gravada: %u vinculo(s)%s\n", NODE_ID,
                static_cast<unsigned>(count), ok ? "" : " FALHA_NVS");
}

// Tempo decorrido desde um instante gravado pela tarefa do BLE. Se ela gravar
// um instante mais novo que o `now` lido antes, a diferença sem sinal daria
// um valor enorme; aqui ela vale zero.
uint32_t elapsedSince(uint32_t now, uint32_t then) {
  const int32_t diff = static_cast<int32_t>(now - then);
  return diff < 0 ? 0u : static_cast<uint32_t>(diff);
}

void sendAssociationStatus(const Binding& binding, uint8_t stateOverride = 0) {
  if (NODE_ID == 0) return;

  const uint16_t baseId = CAN_ID_WIRELESS_ASSOC_STATUS_BASE + static_cast<uint16_t>(NODE_ID) * 2u;
  const uint8_t seq = ++assocStatusSeq;
  const uint8_t state = stateOverride ? stateOverride : binding.assocState;
  const bool active = state != WIRELESS_ASSOC_UNBOUND && state != WIRELESS_ASSOC_REJECTED;
  const bool secure = active && wirelessLinkIsSecure(binding.uuid);

  // O próprio Node não recebe o que transmite: a tabela é alimentada aqui.
  ioc_fo_on_status(&failoverTable, millis(), NODE_ID, binding.childId, binding.uuid, state,
                   binding.autoFailover, secure);

  CANMessage a;
  a.id = baseId;
  a.len = 8;
  a.data[0] = seq;
  a.data[1] = binding.childId;
  a.data[2] = ioc_assoc_pack(state, binding.autoFailover && active, secure);
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

  Serial.printf("[NODE %u] [WIRELESS ASSOC] child=%u uuid=0x%016llX state=%s rssi=%d failover=%s session=%s\n",
                NODE_ID,
                static_cast<unsigned>(binding.childId),
                static_cast<unsigned long long>(binding.uuid),
                assocStateName(state),
                static_cast<int>(binding.rssi),
                binding.autoFailover ? "AUTO" : "MANUAL",
                secure ? "SECURE" : "NONE");
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

// `setPolicy`: o pedido informa a política (BIND_AUTO ou reassociação); um
// BIND simples sobre um vínculo existente não a altera.
void processBindRequest(uint64_t uuid, bool autoFailover, bool setPolicy, uint32_t maxCandidateAgeMs) {
  if (NODE_ID == 0) return;
  const uint32_t now = millis();

  // Pedido para um sensor cujo vínculo gravado ainda aguardava a retomada:
  // o pedido prevalece, e o número de filho gravado é mantido se possível.
  const int pendingIndex = findPendingRestore(uuid);
  const bool hadPending = pendingIndex >= 0;
  ioc_bs_entry pending{};
  if (hadPending) pending = takePendingRestore(pendingIndex);
  const uint8_t preferredChild = hadPending ? pending.child_id : 0;

  portENTER_CRITICAL(&candidateMux);
  int existingIndex = findBindingByUuid(uuid);
  if (existingIndex >= 0) {
    const bool policyChanged = setPolicy && bindings[existingIndex].autoFailover != autoFailover;
    if (setPolicy) bindings[existingIndex].autoFailover = autoFailover;
    bindings[existingIndex].lastPublishMs = now;
    Binding snapshot = bindings[existingIndex];
    portEXIT_CRITICAL(&candidateMux);
    sendAssociationStatus(snapshot);
    if (policyChanged || hadPending) saveBindings();
    return;
  }

  const int candidateIndex = findCandidate(uuid);
  if (candidateIndex < 0 || elapsedSince(now, candidates[candidateIndex].lastSeenMs) > maxCandidateAgeMs) {
    portEXIT_CRITICAL(&candidateMux);
    // Recusado: o vínculo gravado, se havia, continua à espera da retomada.
    if (hadPending) pendingRestore[pendingRestoreCount++] = pending;
    sendRejectedAssociation(uuid, 0, 0, -127);
    return;
  }

  const int bindingIndex = allocateBinding();
  const uint8_t childId =
      preferredChild != 0 && !childIdInUse(preferredChild) ? preferredChild : allocateChildId();
  if (bindingIndex < 0 || childId == 0) {
    Candidate c = candidates[candidateIndex];
    portEXIT_CRITICAL(&candidateMux);
    if (hadPending) pendingRestore[pendingRestoreCount++] = pending;
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
  binding.lastPublishMs = now;
  binding.autoFailover = autoFailover;
  Binding snapshot = binding;
  portEXIT_CRITICAL(&candidateMux);

  sendAssociationStatus(snapshot);
  // O vínculo lógico passa a ter plano de dados: o Node anuncia a oferta
  // autenticada e abre o ponto de acesso para este sensor.
  wirelessLinkOnBound(uuid, childId);
  saveBindings();
}

void processPolicyRequest(uint64_t uuid, bool autoFailover) {
  if (NODE_ID == 0) return;

  portENTER_CRITICAL(&candidateMux);
  const int bindingIndex = findBindingByUuid(uuid);
  if (bindingIndex < 0) {
    portEXIT_CRITICAL(&candidateMux);
    sendRejectedAssociation(uuid, 0, 0, -127);
    return;
  }
  const bool changed = bindings[bindingIndex].autoFailover != autoFailover;
  bindings[bindingIndex].autoFailover = autoFailover;
  bindings[bindingIndex].lastPublishMs = millis();
  Binding snapshot = bindings[bindingIndex];
  portEXIT_CRITICAL(&candidateMux);

  sendAssociationStatus(snapshot);
  if (changed) saveBindings();
}

void processUnbindRequest(uint64_t uuid) {
  if (NODE_ID == 0) return;

  const int pendingIndex = findPendingRestore(uuid);
  if (pendingIndex >= 0) {
    // Desassociado durante a ausência do Node: o vínculo gravado não volta.
    const ioc_bs_entry entry = takePendingRestore(pendingIndex);
    Binding released;
    released.used = true;
    released.uuid = entry.uuid;
    released.childId = entry.child_id;
    released.profile = entry.profile;
    released.protocol = entry.protocol;
    released.assocState = WIRELESS_ASSOC_UNBOUND;
    sendAssociationStatus(released, WIRELESS_ASSOC_UNBOUND);
    saveBindings();
    return;
  }

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

  wirelessLinkOnUnbound(uuid, snapshot.childId);
  sendAssociationStatus(snapshot, WIRELESS_ASSOC_UNBOUND);
  saveBindings();
}

void processAssociationCommand(uint8_t action, uint64_t uuid) {
  switch (action) {
    case WIRELESS_ASSOC_ACTION_BIND:
      processBindRequest(uuid, false, false, BIND_CANDIDATE_MAX_AGE_MS);
      break;
    case WIRELESS_ASSOC_ACTION_BIND_AUTO:
      processBindRequest(uuid, true, true, BIND_CANDIDATE_MAX_AGE_MS);
      break;
    case WIRELESS_ASSOC_ACTION_FAILOVER_OWNER_LOST:
    case WIRELESS_ASSOC_ACTION_FAILOVER_SENSOR_LOST:
      processBindRequest(uuid, true, true, FAILOVER_CANDIDATE_MAX_AGE_MS);
      break;
    case WIRELESS_ASSOC_ACTION_UNBIND:
      processUnbindRequest(uuid);
      break;
    case WIRELESS_ASSOC_ACTION_POLICY_AUTO:
      processPolicyRequest(uuid, true);
      break;
    case WIRELESS_ASSOC_ACTION_POLICY_MANUAL:
      processPolicyRequest(uuid, false);
      break;
    default:
      break;
  }
}

// Outro Node publicou vínculo para um sensor que este Node também mantém.
void resolveBindingConflict(uint8_t otherNode, uint64_t uuid, uint8_t otherState, bool otherSecure) {
  if (NODE_ID == 0) return;

  // Outro Node assumiu, durante a ausência deste, um sensor que ele ia retomar.
  const int pendingIndex = findPendingRestore(uuid);
  if (pendingIndex >= 0 && otherNode != NODE_ID &&
      (otherState == WIRELESS_ASSOC_ASSOCIATING || otherState == WIRELESS_ASSOC_BOUND ||
       otherState == WIRELESS_ASSOC_ONLINE)) {
    takePendingRestore(pendingIndex);
    Serial.printf("[NODE %u] [WIRELESS ASSOC] uuid=0x%016llX assumido pelo NODE %u na ausencia deste; vinculo gravado descartado\n",
                  NODE_ID, static_cast<unsigned long long>(uuid), static_cast<unsigned>(otherNode));
    saveBindings();
    return;
  }

  portENTER_CRITICAL(&candidateMux);
  const int bindingIndex = findBindingByUuid(uuid);
  uint8_t ownState = bindingIndex >= 0 ? bindings[bindingIndex].assocState : 0;
  const bool restored = bindingIndex >= 0 && bindings[bindingIndex].restored;
  portEXIT_CRITICAL(&candidateMux);
  if (bindingIndex < 0) return;

  const bool ownSecure = wirelessLinkIsSecure(uuid);
  // Um vínculo retomado da NVS ainda sem sessão é o mais antigo da disputa:
  // conta como fora de alcance e cede a quem o líder escolheu depois.
  if (restored && !ownSecure) ownState = WIRELESS_ASSOC_STALE;
  if (ioc_fo_should_yield(NODE_ID, ownState, ownSecure, otherNode, otherState, otherSecure)) {
    Serial.printf("[NODE %u] [WIRELESS ASSOC] uuid=0x%016llX cedido ao NODE %u\n", NODE_ID,
                  static_cast<unsigned long long>(uuid), static_cast<unsigned>(otherNode));
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
  ioc_fo_on_observation(&failoverTable, millis(), NODE_ID, c.uuid, c.rssi);

  Serial.printf("[NODE %u] [BLE] candidate uuid=0x%016llX profile=%s rssi=%d protocol=%u\n",
                NODE_ID, static_cast<unsigned long long>(c.uuid), profileName(c.profile),
                static_cast<int>(c.rssi), static_cast<unsigned>(c.protocol));
}

uint64_t candidateUuid(const DiscoveryAssembly& as, const CANMessage& b) {
  uint64_t uuid = 0;
  for (uint8_t i = 0; i < 7; ++i) uuid = (uuid << 8) | as.uuidFirst7[i];
  return (uuid << 8) | b.data[1];
}

uint64_t statusUuid(const AssocStatusAssembly& as, const CANMessage& b) {
  uint64_t uuid = 0;
  for (uint8_t i = 0; i < 4; ++i) uuid = (uuid << 8) | as.uuidFirst4[i];
  for (uint8_t i = 0; i < 4; ++i) uuid = (uuid << 8) | b.data[4 + i];
  return uuid;
}

void printProbeCandidate(uint8_t reporter, const DiscoveryAssembly& as, const CANMessage& b) {
  const uint64_t uuid = candidateUuid(as, b);
  const uint8_t profile = b.data[2];
  const int8_t rssi = static_cast<int8_t>(b.data[3]);
  const uint8_t protocol = b.data[4];

  Serial.printf("[GW] WIRELESS_CANDIDATE reporter=%u uuid=0x%016llX profile=%s rssi=%d protocol=%u\n",
                reporter, static_cast<unsigned long long>(uuid), profileName(profile),
                static_cast<int>(rssi), static_cast<unsigned>(protocol));
}

void printProbeAssociation(uint8_t reporter, const AssocStatusAssembly& as, const CANMessage& b) {
  const uint64_t uuid = statusUuid(as, b);
  const int8_t rssi = static_cast<int8_t>(b.data[2]);
  const uint8_t protocol = b.data[3];
  uint8_t assocState = 0;
  int autoFailover = 0;
  int secure = 0;
  ioc_assoc_unpack(as.state, &assocState, &autoFailover, &secure);

  Serial.printf("[GW] WIRELESS_ASSOC node=%u child=%u uuid=0x%016llX profile=%s state=%s rssi=%d protocol=%u "
                "failover=%s session=%s\n",
                reporter,
                static_cast<unsigned>(as.childId),
                static_cast<unsigned long long>(uuid),
                profileName(as.profile),
                assocStateName(assocState),
                static_cast<int>(rssi),
                static_cast<unsigned>(protocol),
                autoFailover ? "AUTO" : "MANUAL",
                secure ? "SECURE" : "NONE");
}

bool parseUuid(const char* text, uint64_t& uuid) {
  if (!text || !*text) return false;
  char* end = nullptr;
  uuid = strtoull(text, &end, 0);
  return end && *end == '\0' && uuid != 0;
}

const char* assocActionName(uint8_t action) {
  switch (action) {
    case WIRELESS_ASSOC_ACTION_BIND:          return "BIND";
    case WIRELESS_ASSOC_ACTION_BIND_AUTO:     return "BIND";
    case WIRELESS_ASSOC_ACTION_UNBIND:        return "UNBIND";
    case WIRELESS_ASSOC_ACTION_POLICY_AUTO:   return "POLICY";
    case WIRELESS_ASSOC_ACTION_POLICY_MANUAL: return "POLICY";
    default:                                  return "COMMAND";
  }
}

uint8_t transmitAssociationCommand(uint8_t target, uint8_t action, uint64_t uuid, const FailoverInfo& info) {
  const uint8_t seq = ++assocCommandSeq;
  // A Probe 00 e o líder transmitem em identificadores diferentes.
  const bool fromProbe = NODE_ID == 0;

  CANMessage a;
  a.id = fromProbe ? CAN_ID_WIRELESS_ASSOC_CMD_A : CAN_ID_WIRELESS_ASSOC_LEADER_A;
  a.len = 8;
  a.data[0] = seq;
  a.data[1] = target;
  a.data[2] = action;
  for (uint8_t i = 0; i < 5; ++i) {
    a.data[3 + i] = static_cast<uint8_t>((uuid >> (56 - 8 * i)) & 0xFFu);
  }

  CANMessage b;
  b.id = fromProbe ? CAN_ID_WIRELESS_ASSOC_CMD_B : CAN_ID_WIRELESS_ASSOC_LEADER_B;
  b.len = 8;
  b.data[0] = seq;
  b.data[1] = target;
  b.data[2] = action;
  for (uint8_t i = 0; i < 3; ++i) {
    b.data[3 + i] = static_cast<uint8_t>((uuid >> (16 - 8 * i)) & 0xFFu);
  }
  b.data[6] = info.fromNode;
  b.data[7] = static_cast<uint8_t>(info.rssi);

  if (can.tryToSend(a)) netMetricsOnTransmit(a);
  if (can.tryToSend(b)) netMetricsOnTransmit(b);
  return seq;
}

// Comando do operador, emitido pela Probe 00.
void sendAssociationCommand(uint8_t target, uint8_t action, uint64_t uuid) {
  const uint8_t seq = transmitAssociationCommand(target, action, uuid, FailoverInfo{});
  const char* policy = "";
  if (action == WIRELESS_ASSOC_ACTION_BIND_AUTO || action == WIRELESS_ASSOC_ACTION_POLICY_AUTO) {
    policy = " failover=AUTO";
  } else if (action == WIRELESS_ASSOC_ACTION_POLICY_MANUAL) {
    policy = " failover=MANUAL";
  }
  Serial.printf("[GW] WIRELESS_%s_TX node=%u uuid=0x%016llX seq=%u%s\n",
                assocActionName(action),
                static_cast<unsigned>(target),
                static_cast<unsigned long long>(uuid),
                static_cast<unsigned>(seq),
                policy);
}

// Comando emitido por um Node (o líder). O quadro sempre vai ao barramento,
// para que a Probe 00 registre a decisão; quando o destino é o próprio Node,
// que não recebe o que transmite, o pedido é executado em seguida.
void dispatchAssociationCommand(uint8_t target, uint8_t action, uint64_t uuid, const FailoverInfo& info) {
  transmitAssociationCommand(target, action, uuid, info);
  if (target == NODE_ID) processAssociationCommand(action, uuid);
}

void pollFailover() {
  if (NODE_ID == 0) return;
  const uint32_t now = millis();
  if (now - lastFailoverPollMs < FAILOVER_POLL_MS) return;
  lastFailoverPollMs = now;

  // Todos os Nodes mantêm a tabela; só o líder em exercício decide.
  if (state != STATE_LEADER) return;
  ioc_fo_decision decision;
  if (!ioc_fo_poll(&failoverTable, now, &decision)) return;

  const bool ownerLost = decision.reason == IOC_FO_REASON_OWNER_LOST;
  Serial.printf("[NODE %u] [WIRELESS FAILOVER] uuid=0x%016llX from=%u to=%u reason=%s rssi=%d\n", NODE_ID,
                static_cast<unsigned long long>(decision.uuid), static_cast<unsigned>(decision.from_node),
                static_cast<unsigned>(decision.to_node), ownerLost ? "OWNER_LOST" : "SENSOR_LOST",
                static_cast<int>(decision.rssi));

  // O responsável anterior não é desassociado aqui. Se estiver ativo, ele
  // libera o sensor quando ouvir o novo Node publicar o vínculo; se o novo
  // recusar ou estiver fora do ar, o vínculo anterior continua existindo e
  // a tabela permite outra tentativa depois do intervalo mínimo.
  FailoverInfo info;
  info.fromNode = decision.from_node;
  info.rssi = decision.rssi;
  dispatchAssociationCommand(decision.to_node,
                             ownerLost ? WIRELESS_ASSOC_ACTION_FAILOVER_OWNER_LOST
                                       : WIRELESS_ASSOC_ACTION_FAILOVER_SENSOR_LOST,
                             decision.uuid, info);
}

void pollBindings() {
  if (NODE_ID == 0) return;

  for (uint8_t i = 0; i < MAX_BINDINGS; ++i) {
    Binding snapshot;
    bool publish = false;

    portENTER_CRITICAL(&candidateMux);
    // Lido dentro da seção crítica: a tarefa do BLE não grava um instante
    // posterior a este enquanto os estados são avaliados.
    const uint32_t now = millis();
    Binding& binding = bindings[i];
    if (!binding.used) {
      portEXIT_CRITICAL(&candidateMux);
      continue;
    }

    // Uma sessão de dados ativa prova a presença do sensor mesmo que o rádio
    // BLE, dividido com o ponto de acesso, deixe de ouvir os anúncios dele.
    const bool secure = wirelessLinkIsSecure(binding.uuid);
    if (secure) binding.lastSeenMs = now;
    if (secure) binding.restored = false;  // vínculo retomado da NVS confirmado pela sessão
    const uint32_t age = elapsedSince(now, binding.lastSeenMs);
    if (binding.assocState == WIRELESS_ASSOC_ASSOCIATING &&
        now - binding.stateSinceMs >= ASSOCIATING_MIN_MS &&
        age <= ONLINE_STALE_MS) {
      binding.assocState = WIRELESS_ASSOC_BOUND;
      binding.stateSinceMs = now;
      snapshot = binding;
      publish = true;
    } else if (binding.assocState == WIRELESS_ASSOC_ASSOCIATING && age > ONLINE_STALE_MS) {
      // O sensor não foi mais ouvido desde o pedido: o vínculo não fica
      // indefinidamente em ASSOCIATING, o que impediria outra reassociação.
      binding.assocState = WIRELESS_ASSOC_STALE;
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
    // A situação da sessão segue no estado publicado: o líder precisa dela
    // para decidir, e os demais Nodes para resolver vínculos duplicados.
    if (!publish && (secure != binding.publishedSecure || now - binding.lastPublishMs >= ASSOC_REFRESH_MS)) {
      snapshot = binding;
      publish = true;
    }
    if (publish) {
      binding.lastPublishMs = now;
      binding.publishedSecure = secure;
    }
    portEXIT_CRITICAL(&candidateMux);

    if (publish) sendAssociationStatus(snapshot);
  }
}

// Lê os vínculos gravados (primeira passagem do laço, com CAN e enlace já
// iniciados). Nada é retomado antes de RESTORE_GRACE_MS.
void loadPendingRestore() {
  restoreLoaded = true;
  if (NODE_ID == 0) return;
  uint8_t blob[IOC_BS_MAX_BYTES];
  size_t len = 0;
  Preferences prefs;
  // Aberto em escrita: no primeiro boot o espaço de nomes ainda não existe.
  if (prefs.begin(NVS_NAMESPACE, false)) {
    len = prefs.getBytes(NVS_KEY, blob, sizeof(blob));
    prefs.end();
  }
  if (len == 0) return;
  uint8_t count = 0;
  if (!ioc_bs_decode(blob, len, NODE_ID, pendingRestore, MAX_BINDINGS, &count)) {
    Serial.printf("[NODE %u] [WIRELESS ASSOC] tabela gravada invalida; nenhum vinculo retomado\n", NODE_ID);
    return;
  }
  pendingRestoreCount = count;
  restoreDeadlineMs = millis() + RESTORE_GRACE_MS;
  if (count) {
    Serial.printf("[NODE %u] [WIRELESS ASSOC] %u vinculo(s) gravado(s); retomada em %lu ms se nenhum outro Node os assumiu\n",
                  NODE_ID, static_cast<unsigned>(count), static_cast<unsigned long>(RESTORE_GRACE_MS));
  }
}

// Fim da espera: os vínculos gravados que ninguém assumiu voltam como BOUND,
// com o mesmo número de filho, e o Node volta a oferecer o plano de dados.
void finishRestore() {
  ioc_bs_entry entries[MAX_BINDINGS];
  const uint8_t count = pendingRestoreCount;
  memcpy(entries, pendingRestore, sizeof(ioc_bs_entry) * count);
  pendingRestoreCount = 0;
  bool changed = false;

  for (uint8_t k = 0; k < count; ++k) {
    const ioc_bs_entry& entry = entries[k];
    portENTER_CRITICAL(&candidateMux);
    const uint32_t now = millis();
    if (findBindingByUuid(entry.uuid) >= 0) {
      portEXIT_CRITICAL(&candidateMux);
      changed = true;
      continue;
    }
    const int bindingIndex = allocateBinding();
    const uint8_t childId = childIdInUse(entry.child_id) ? allocateChildId() : entry.child_id;
    if (bindingIndex < 0 || childId == 0) {
      portEXIT_CRITICAL(&candidateMux);
      changed = true;
      continue;
    }
    Binding& binding = bindings[bindingIndex];
    binding = Binding{};
    binding.used = true;
    binding.uuid = entry.uuid;
    binding.childId = childId;
    binding.profile = entry.profile;
    binding.protocol = entry.protocol;
    binding.rssi = -127;
    binding.assocState = WIRELESS_ASSOC_BOUND;
    binding.lastSeenMs = now;
    binding.stateSinceMs = now;
    binding.lastPublishMs = now - ASSOC_REFRESH_MS;  // publica na próxima passagem
    binding.autoFailover = (entry.flags & IOC_BS_FLAG_AUTO_FAILOVER) != 0;
    binding.restored = true;
    const bool autoFailover = binding.autoFailover;
    portEXIT_CRITICAL(&candidateMux);

    if (childId != entry.child_id) changed = true;
    Serial.printf("[NODE %u] [WIRELESS ASSOC] vinculo retomado da NVS child=%u uuid=0x%016llX failover=%s\n",
                  NODE_ID, static_cast<unsigned>(childId), static_cast<unsigned long long>(entry.uuid),
                  autoFailover ? "AUTO" : "MANUAL");
    wirelessLinkOnBound(entry.uuid, childId);
  }
  if (changed) saveBindings();
}

} // namespace

void wirelessDiscoveryInit() {
  ioc_fo_init(&failoverTable);
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
    if (!restoreLoaded) loadPendingRestore();
    if (pendingRestoreCount && static_cast<int32_t>(millis() - restoreDeadlineMs) >= 0) finishRestore();
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
  pollFailover();
}

void wirelessFailoverOnNodeFault(uint8_t nodeId) {
  if (NODE_ID == 0) return;
  ioc_fo_on_node_fault(&failoverTable, nodeId);
}

void wirelessFailoverOnLocalStall() {
  if (NODE_ID == 0) return;
  ioc_fo_on_local_stall(&failoverTable, millis());
  lastFailoverPollMs = millis();
}

bool wirelessAssociationHandleSerialCommand(const char* command) {
  if (NODE_ID != 0 || !command) return false;

  char verb[16] = {0};
  unsigned int target = 0;
  char uuidText[32] = {0};
  char option[12] = {0};
  const int fields = sscanf(command, "WIRELESS %15s %u %31s %11s", verb, &target, uuidText, option);
  if (fields < 3) {
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

  const bool optionAuto = strcasecmp(option, "AUTO") == 0;
  const bool optionManual = strcasecmp(option, "MANUAL") == 0;
  if (fields == 4 && !optionAuto && !optionManual) {
    Serial.println("[GW] WIRELESS_ERROR opcao invalida; use AUTO ou MANUAL");
    return true;
  }

  if (strcasecmp(verb, "BIND") == 0) {
    sendAssociationCommand(static_cast<uint8_t>(target),
                           optionAuto ? WIRELESS_ASSOC_ACTION_BIND_AUTO : WIRELESS_ASSOC_ACTION_BIND, uuid);
    return true;
  }
  if (strcasecmp(verb, "UNBIND") == 0) {
    sendAssociationCommand(static_cast<uint8_t>(target), WIRELESS_ASSOC_ACTION_UNBIND, uuid);
    return true;
  }
  if (strcasecmp(verb, "POLICY") == 0) {
    if (fields != 4) {
      Serial.println("[GW] WIRELESS_ERROR use: WIRELESS POLICY <node> <uuid> AUTO|MANUAL");
      return true;
    }
    sendAssociationCommand(static_cast<uint8_t>(target),
                           optionAuto ? WIRELESS_ASSOC_ACTION_POLICY_AUTO : WIRELESS_ASSOC_ACTION_POLICY_MANUAL,
                           uuid);
    return true;
  }

  Serial.println("[GW] WIRELESS_ERROR use: WIRELESS BIND <node> <uuid> [AUTO] | WIRELESS UNBIND <node> <uuid> | "
                 "WIRELESS POLICY <node> <uuid> AUTO|MANUAL");
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

    if (as.havePartA && as.seq == rx.data[0]) {
      if (NODE_ID == 0) {
        printProbeCandidate(reporter, as, rx);
      } else {
        ioc_fo_on_observation(&failoverTable, millis(), reporter, candidateUuid(as, rx),
                              static_cast<int8_t>(rx.data[3]));
      }
    }
    as.havePartA = false;
    return true;
  }

  // Os comandos são remontados por todos: o Node de destino os executa e a
  // Probe 00 registra as reassociações decididas pelo líder. Os pedidos da
  // Probe e os do líder chegam em pares de identificadores distintos.
  if (rx.id == CAN_ID_WIRELESS_ASSOC_CMD_A || rx.id == CAN_ID_WIRELESS_ASSOC_LEADER_A) {
    if (rx.len != 8) return true;
    AssocCommandAssembly& assembly =
        rx.id == CAN_ID_WIRELESS_ASSOC_CMD_A ? assocCommandAssembly : leaderCommandAssembly;
    assembly.havePartA = true;
    assembly.seq = rx.data[0];
    assembly.target = rx.data[1];
    assembly.action = rx.data[2];
    memcpy(assembly.uuidFirst5, &rx.data[3], 5);
    return true;
  }

  if (rx.id == CAN_ID_WIRELESS_ASSOC_CMD_B || rx.id == CAN_ID_WIRELESS_ASSOC_LEADER_B) {
    if (rx.len != 8) return true;
    const bool fromLeader = rx.id == CAN_ID_WIRELESS_ASSOC_LEADER_B;
    AssocCommandAssembly& assembly = fromLeader ? leaderCommandAssembly : assocCommandAssembly;
    if (!assembly.havePartA ||
        assembly.seq != rx.data[0] ||
        assembly.target != rx.data[1] ||
        assembly.action != rx.data[2]) {
      assembly.havePartA = false;
      return true;
    }

    uint64_t uuid = 0;
    for (uint8_t i = 0; i < 5; ++i) uuid = (uuid << 8) | assembly.uuidFirst5[i];
    for (uint8_t i = 0; i < 3; ++i) uuid = (uuid << 8) | rx.data[3 + i];
    const uint8_t action = assembly.action;
    const uint8_t target = assembly.target;
    assembly.havePartA = false;

    const bool failover = action == WIRELESS_ASSOC_ACTION_FAILOVER_OWNER_LOST ||
                          action == WIRELESS_ASSOC_ACTION_FAILOVER_SENSOR_LOST;
    // O líder só emite reassociações; os demais pedidos são da Probe 00.
    if (fromLeader != failover) return true;

    if (NODE_ID == 0) {
      if (failover) {
        Serial.printf("[GW] WIRELESS_FAILOVER uuid=0x%016llX from=%u to=%u reason=%s rssi=%d\n",
                      static_cast<unsigned long long>(uuid), static_cast<unsigned>(rx.data[6]),
                      static_cast<unsigned>(target),
                      action == WIRELESS_ASSOC_ACTION_FAILOVER_OWNER_LOST ? "OWNER_LOST" : "SENSOR_LOST",
                      static_cast<int>(static_cast<int8_t>(rx.data[7])));
      }
      return true;
    }
    if (target == NODE_ID) processAssociationCommand(action, uuid);
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

    if (as.havePartA && as.seq == rx.data[0] && as.childId == rx.data[1]) {
      if (NODE_ID == 0) {
        printProbeAssociation(reporter, as, rx);
      } else {
        const uint64_t uuid = statusUuid(as, rx);
        uint8_t assocState = 0;
        int autoFailover = 0;
        int secure = 0;
        ioc_assoc_unpack(as.state, &assocState, &autoFailover, &secure);
        ioc_fo_on_status(&failoverTable, millis(), reporter, as.childId, uuid, assocState, autoFailover, secure);
        resolveBindingConflict(reporter, uuid, assocState, secure != 0);
      }
    }
    as.havePartA = false;
    return true;
  }

  return false;
}
