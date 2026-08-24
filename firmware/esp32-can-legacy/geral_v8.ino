#include <Arduino.h>
#include <SPI.h>
#include <ACAN2515.h>

#include "can_ids.h"
#include "protocolo.h"
#include "node_types.h"
#include "node_config.h"
#include "comandos.h"
#include "falhas.h"

/* =========================================================
 * CONFIGURACAO LOCAL DO NO
 * ========================================================= */

const uint8_t NODE_ID = 4;   // NODE_ID 0 = GATEWAY
                             // NODE_ID > 0 = no funcional

/* =========================================================
 * PARAMETROS GERAIS
 * ========================================================= */

#define MAX_KNOWN_NODES        32
#define DISCOVERY_DURATION_MS  5000
#define RECOVERY_DURATION_MS   1500

#define HEARTBEAT_LED_MS       80

#define GATEWAY_LEADER_TIMEOUT_MULTIPLIER  4
#define NODE_ABSENCE_HEARTBEATS            5
#define GATEWAY_STATUS_REQUEST_DELAY_MS    1000
#define GATEWAY_STATUS_REFRESH_MS          10000

#define JOIN_RETRY_MS          2000
#define JOIN_MAX_ATTEMPTS      5

#define REPLICA_SYNC_MODULO    83

/*
 * Em fase de teste, manter true.
 * Para apresentacao com saida centralizada no Gateway, usar false.
 */
#define DEBUG_LOCAL            true

/* =========================================================
 * LEDS
 * ========================================================= */

#define LED_LEADER  25
#define LED_FAULT   26
#define LED_STATUS  27

/*
 * LEDS COM LOGICA INVERTIDA
 *
 * Nesta montagem, os LEDs acendem com LOW e apagam com HIGH.
 */
#define LED_ON      LOW
#define LED_OFF     HIGH

/* =========================================================
 * CAN / MCP2515
 * ========================================================= */

static const byte MCP_CS  = 5;
static const byte MCP_INT = 4;

static const uint32_t MCP_QUARTZ_HZ = 8000000;
static const uint32_t CAN_BITRATE   = 500UL * 1000UL;

ACAN2515 can(MCP_CS, SPI, MCP_INT);

/* =========================================================
 * ESTADO GLOBAL
 * ========================================================= */

NodeState state = STATE_IDLE;

uint8_t leaderId = 0;
uint8_t heartbeatTick = 0;

uint8_t heartbeatRateMode = HB_RATE_NORMAL;
unsigned long heartbeatPeriodMs = 1000;

unsigned long bootTime = 0;
unsigned long electionStartTime = 0;
unsigned long recoveryStartTime = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastHeartbeatLedPulse = 0;

bool faultDetected = false;

/* =========================================================
 * ESTADO DE JOIN
 * ========================================================= */

unsigned long lastJoinRequestTime = 0;

uint8_t joinAttempts = 0;
bool joinAccepted = false;
bool waitingFirstHeartbeatAfterJoin = false;

/* =========================================================
 * ESTADO DO GATEWAY
 * ========================================================= */

uint8_t gatewayObservedLeader = 0;
uint8_t gatewayLastHeartbeatTick = 0;

unsigned long gatewayLastHeartbeatTime = 0;
unsigned long gatewayLastStatusRequestTime = 0;

bool gatewayHasLeader = false;
bool gatewayRecoverySent = false;
bool gatewayInitialStatusRequested = false;

/* =========================================================
 * TABELA DE NOS DESCOBERTOS
 * ========================================================= */

uint8_t knownNodes[MAX_KNOWN_NODES];
uint8_t knownNodeCount = 0;

/* =========================================================
 * TABELA LOCAL DE STATUS DA REDE
 * ========================================================= */

uint8_t networkNodeIds[MAX_KNOWN_NODES];
uint8_t networkNodeStatus[MAX_KNOWN_NODES];
uint8_t networkLastValue[MAX_KNOWN_NODES];
uint8_t networkLastTick[MAX_KNOWN_NODES];
unsigned long networkLastSeenMs[MAX_KNOWN_NODES];
bool networkKnown[MAX_KNOWN_NODES];

/* =========================================================
 * TABELA DE STATUS
 * ========================================================= */

void initNetworkStatusTable() {
  for (uint8_t i = 0; i < MAX_KNOWN_NODES; i++) {
    networkNodeIds[i] = 0;
    networkNodeStatus[i] = STATUS_UNKNOWN;
    networkLastValue[i] = 0x00;
    networkLastTick[i] = 0x00;
    networkLastSeenMs[i] = 0;
    networkKnown[i] = false;
  }
}

int findNetworkStatusIndex(uint8_t nodeId) {
  for (uint8_t i = 0; i < MAX_KNOWN_NODES; i++) {
    if (networkKnown[i] && networkNodeIds[i] == nodeId) {
      return i;
    }
  }

  return -1;
}

int registerNetworkStatusNode(uint8_t nodeId) {
  int idx = findNetworkStatusIndex(nodeId);

  if (idx >= 0) {
    return idx;
  }

  for (uint8_t i = 0; i < MAX_KNOWN_NODES; i++) {
    if (!networkKnown[i]) {
      networkKnown[i] = true;
      networkNodeIds[i] = nodeId;
      networkNodeStatus[i] = STATUS_UNKNOWN;
      networkLastValue[i] = 0x00;
      networkLastTick[i] = 0x00;
      networkLastSeenMs[i] = millis();
      return i;
    }
  }

  return -1;
}

void markNodeSeen(uint8_t nodeId) {
  int idx = registerNetworkStatusNode(nodeId);

  if (idx < 0) {
    return;
  }

  networkLastSeenMs[idx] = millis();
}

bool isProtectedStatusTransition(uint8_t currentStatus, uint8_t newStatus) {
  /*
   * Regra central:
   * uma falha confirmada nao pode ser apagada por eleicao,
   * resposta de status, JOIN ou atualizacao normal.
   *
   * A unica transicao FAULT -> OK permitida ocorre via
   * forceNetworkStatusLocal(), usada exclusivamente pelo CLEAR_FAULT.
   */
  if (currentStatus == STATUS_FOLLOWER_FAULT &&
      newStatus != STATUS_FOLLOWER_FAULT) {
    return true;
  }

  return false;
}

void updateNetworkStatusLocal(uint8_t nodeId, uint8_t status) {
  int idx = registerNetworkStatusNode(nodeId);

  if (idx < 0) {
    if (state == STATE_GATEWAY || DEBUG_LOCAL) {
      Serial.println("[STATUS] Tabela de status cheia");
    }
    return;
  }

  if (isProtectedStatusTransition(networkNodeStatus[idx], status)) {
    if (state == STATE_GATEWAY || DEBUG_LOCAL) {
      Serial.print("[STATUS] Transicao protegida ignorada para NODE ");
      Serial.println(nodeId);
    }
    return;
  }

  networkNodeStatus[idx] = status;
  networkLastSeenMs[idx] = millis();
}

void forceNetworkStatusLocal(uint8_t nodeId, uint8_t status) {
  int idx = registerNetworkStatusNode(nodeId);

  if (idx < 0) {
    if (state == STATE_GATEWAY || DEBUG_LOCAL) {
      Serial.println("[STATUS] Tabela de status cheia");
    }
    return;
  }

  networkNodeStatus[idx] = status;
  networkLastSeenMs[idx] = millis();
}

void updateNetworkSensorValue(uint8_t nodeId, uint8_t value, uint8_t tick) {
  int idx = registerNetworkStatusNode(nodeId);

  if (idx < 0) {
    if (state == STATE_GATEWAY || DEBUG_LOCAL) {
      Serial.println("[STATUS] Tabela de status cheia");
    }
    return;
  }

  networkLastValue[idx] = value;
  networkLastTick[idx] = tick;
  networkLastSeenMs[idx] = millis();
}

uint8_t getReplicatedStatus(uint8_t nodeId) {
  int idx = findNetworkStatusIndex(nodeId);

  if (idx < 0) {
    return STATUS_UNKNOWN;
  }

  return networkNodeStatus[idx];
}

bool isActiveSensorStatus(uint8_t status) {
  return status == STATUS_FOLLOWER_OK;
}

bool isNodeActiveForTdma(uint8_t nodeId) {
  if (nodeId == 0) return false;
  if (nodeId == leaderId) return false;

  uint8_t status = getReplicatedStatus(nodeId);

  return isActiveSensorStatus(status);
}

uint8_t getActiveSensorCount() {
  uint8_t count = 0;

  for (uint8_t i = 0; i < knownNodeCount; i++) {
    if (isNodeActiveForTdma(knownNodes[i])) {
      count++;
    }
  }

  return count;
}

uint8_t getActiveNodeSlot(uint8_t id) {
  uint8_t slot = 0;

  for (uint8_t i = 0; i < knownNodeCount; i++) {
    uint8_t candidate = knownNodes[i];

    if (candidate < id && isNodeActiveForTdma(candidate)) {
      slot++;
    }
  }

  return slot;
}

void printNetworkStatusLine(uint8_t nodeId, uint8_t status) {
  Serial.print("[STATUS] NODE ");
  Serial.print(nodeId);
  Serial.print(" estado=");

  if (status == STATUS_ROLE_LEADER) {
    Serial.println("LIDER");
  } else if (status == STATUS_FOLLOWER_OK) {
    Serial.println("ATIVO");
  } else if (status == STATUS_FOLLOWER_DISABLED) {
    Serial.println("DESATIVADO");
  } else if (status == STATUS_FOLLOWER_FAULT) {
    Serial.println("FALHA");
  } else if (status == STATUS_GATEWAY) {
    Serial.println("GATEWAY");
  } else if (status == STATUS_JOINING) {
    Serial.println("ENTRANDO");
  } else if (status == STATUS_LEADER_CONFLICT) {
    Serial.println("CONFLITO_LIDER");
  } else {
    Serial.println("DESCONHECIDO");
  }
}

/* =========================================================
 * HEARTBEAT DINAMICO
 * ========================================================= */

unsigned long heartbeatRateToMs(uint8_t mode) {
  switch (mode) {
    case HB_RATE_VERY_SLOW: return 2000;
    case HB_RATE_SLOW:      return 1500;
    case HB_RATE_NORMAL:    return 1000;
    case HB_RATE_FAST:      return 750;
    case HB_RATE_VERY_FAST: return 500;
    default:                return 1000;
  }
}

bool isValidHeartbeatRate(uint8_t mode) {
  return mode >= HB_RATE_VERY_SLOW && mode <= HB_RATE_VERY_FAST;
}

void applyHeartbeatRate(uint8_t mode) {
  if (!isValidHeartbeatRate(mode)) {
    if (state == STATE_GATEWAY || DEBUG_LOCAL) {
      Serial.println("[HB] Modo de heartbeat invalido");
    }
    return;
  }

  heartbeatRateMode = mode;
  heartbeatPeriodMs = heartbeatRateToMs(mode);

  if (state == STATE_GATEWAY || DEBUG_LOCAL) {
    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.print("] [HB] modo=");
    Serial.print(heartbeatRateMode);
    Serial.print(" periodo=");
    Serial.print(heartbeatPeriodMs);
    Serial.println(" ms");
  }
}

/* =========================================================
 * AUXILIARES
 * ========================================================= */

void logNode(const char* tag, const char* msg) {
  if (!(state == STATE_GATEWAY || DEBUG_LOCAL)) return;

  Serial.print("[NODE ");
  Serial.print(NODE_ID);
  Serial.print("] [");
  Serial.print(tag);
  Serial.print("] ");
  Serial.println(msg);
}

bool isGateway() {
  return NODE_ID == 0;
}

bool isKnownNode(uint8_t id) {
  for (uint8_t i = 0; i < knownNodeCount; i++) {
    if (knownNodes[i] == id) return true;
  }

  return false;
}

void registerNode(uint8_t id) {
  if (id == 0) return;
  if (isKnownNode(id)) return;
  if (knownNodeCount >= MAX_KNOWN_NODES) return;

  knownNodes[knownNodeCount++] = id;

  if (state == STATE_GATEWAY || DEBUG_LOCAL) {
    Serial.print(state == STATE_GATEWAY ? "[GW] " : "[DESCOBERTA] ");
    Serial.print("No descoberto: NODE ");
    Serial.println(id);
  }
}

void markNodeObservedFunctional(uint8_t id) {
  if (id == 0) return;

  registerNode(id);

  /*
   * UNKNOWN -> OK e permitido na primeira eleicao.
   * FAULT -> OK e bloqueado pela protecao de status.
   */
  updateNetworkStatusLocal(id, STATUS_FOLLOWER_OK);
  markNodeSeen(id);
}

uint8_t getHighestKnownNodeId() {
  uint8_t highest = NODE_ID;

  for (uint8_t i = 0; i < knownNodeCount; i++) {
    uint8_t id = knownNodes[i];

    if (getReplicatedStatus(id) == STATUS_FOLLOWER_FAULT) {
      continue;
    }

    if (id > highest) {
      highest = id;
    }
  }

  return highest;
}

uint8_t getLocalStatusForReplication() {
  if (state == STATE_GATEWAY) return STATUS_GATEWAY;
  if (state == STATE_LEADER || state == STATE_RECOVERING) return STATUS_ROLE_LEADER;
  if (state == STATE_JOINING) return STATUS_JOINING;
  if (state == STATE_FAULT || faultDetected) return STATUS_FOLLOWER_FAULT;
  if (!sensorEnabled) return STATUS_FOLLOWER_DISABLED;
  return STATUS_FOLLOWER_OK;
}

/* =========================================================
 * LEDS
 * ========================================================= */

void turnAllLedsOff() {
  digitalWrite(LED_LEADER, LED_OFF);
  digitalWrite(LED_STATUS, LED_OFF);
  digitalWrite(LED_FAULT, LED_OFF);
}

void pulseHeartbeatLed() {
  lastHeartbeatLedPulse = millis();
}

void updateLeds() {
  if (state == STATE_IDLE) {
    digitalWrite(LED_LEADER, LED_ON);
    digitalWrite(LED_STATUS, LED_ON);
    digitalWrite(LED_FAULT, LED_ON);
    return;
  }

  if (state == STATE_GATEWAY) {
    digitalWrite(LED_LEADER, LED_OFF);

    if (millis() - lastHeartbeatLedPulse < HEARTBEAT_LED_MS) {
      digitalWrite(LED_STATUS, LED_ON);
    } else {
      digitalWrite(LED_STATUS, LED_OFF);
    }

    digitalWrite(LED_FAULT, gatewayRecoverySent ? LED_ON : LED_OFF);
    return;
  }

  bool electionFinished =
    (state == STATE_LEADER || state == STATE_FOLLOWER || state == STATE_RECOVERING);

  digitalWrite(
    LED_LEADER,
    (electionFinished && (state == STATE_LEADER || state == STATE_RECOVERING)) ? LED_ON : LED_OFF
  );

  if (electionFinished &&
      millis() - lastHeartbeatLedPulse < HEARTBEAT_LED_MS) {
    digitalWrite(LED_STATUS, LED_ON);
  } else {
    digitalWrite(LED_STATUS, LED_OFF);
  }

  if (faultDetected || state == STATE_FAULT || !sensorEnabled) {
    digitalWrite(LED_FAULT, LED_ON);
  } else {
    digitalWrite(LED_FAULT, LED_OFF);
  }
}

/* =========================================================
 * ENVIO DE MENSAGENS
 * ========================================================= */

void sendControlFrame(uint8_t subcmd, uint8_t targetId, uint8_t action) {
  CANMessage msg;

  msg.id = CAN_ID_CONTROL;
  msg.len = 4;

  msg.data[0] = CTRL_OPCODE;
  msg.data[1] = subcmd;
  msg.data[2] = targetId;
  msg.data[3] = action;

  can.tryToSend(msg);
}

void sendStatusFrame(uint8_t subcmd, uint8_t targetId, uint8_t value) {
  CANMessage msg;

  msg.id = CAN_ID_CONTROL;
  msg.len = 4;

  msg.data[0] = STATUS_OPCODE;
  msg.data[1] = subcmd;
  msg.data[2] = targetId;
  msg.data[3] = value;

  can.tryToSend(msg);
}

void publishStateUpdate(uint8_t nodeId, uint8_t status) {
  updateNetworkStatusLocal(nodeId, status);
  sendStatusFrame(CTRL_SUBCMD_STATE_UPDATE, nodeId, status);

  if (state == STATE_GATEWAY || DEBUG_LOCAL) {
    Serial.print("[STATUS TX] node=");
    Serial.print(nodeId);
    Serial.print(" status=");
    Serial.println(status, HEX);
  }
}

void publishStateUpdateForced(uint8_t nodeId, uint8_t status) {
  forceNetworkStatusLocal(nodeId, status);
  sendStatusFrame(CTRL_SUBCMD_STATE_FORCE_UPDATE, nodeId, status);

  if (state == STATE_GATEWAY || DEBUG_LOCAL) {
    Serial.print("[STATUS FORCE TX] node=");
    Serial.print(nodeId);
    Serial.print(" status=");
    Serial.println(status, HEX);
  }
}

void sendHeartbeatRateToTarget(uint8_t targetId) {
  sendControlFrame(
    CTRL_SUBCMD_HEARTBEAT_RATE,
    targetId,
    heartbeatRateMode
  );
}

void sendStatusRequestBroadcast() {
  sendControlFrame(
    CTRL_SUBCMD_STATUS_REQUEST,
    CTRL_TARGET_BROADCAST,
    0x00
  );

  if (state == STATE_GATEWAY || DEBUG_LOCAL) {
    Serial.println("[STATUS TX] Requisicao global de status enviada");
  }
}

/* =========================================================
 * REPLICACAO EM SLOT LOGICO
 * ========================================================= */

bool isReplicaSyncTick(uint8_t tick) {
  return (tick % REPLICA_SYNC_MODULO) == 0;
}

void sendNextReplicaEntry() {
  if (state != STATE_LEADER) return;

  static uint8_t replicaCursor = 0;

  for (uint8_t attempts = 0; attempts < MAX_KNOWN_NODES; attempts++) {
    uint8_t idx = replicaCursor;

    replicaCursor++;
    if (replicaCursor >= MAX_KNOWN_NODES) {
      replicaCursor = 0;
    }

    if (networkKnown[idx]) {
      sendStatusFrame(
        CTRL_SUBCMD_STATE_UPDATE,
        networkNodeIds[idx],
        networkNodeStatus[idx]
      );

      if (DEBUG_LOCAL) {
        Serial.print("[REPLICA TX] node=");
        Serial.print(networkNodeIds[idx]);
        Serial.print(" status=");
        Serial.println(networkNodeStatus[idx], HEX);
      }

      return;
    }
  }
}

/* =========================================================
 * ELEICAO
 * ========================================================= */

void sendElectionAnnouncement() {
  if (state == STATE_GATEWAY) return;

  CANMessage msg;

  msg.id = CAN_ID_ELECTION;
  msg.len = 1;
  msg.data[0] = NODE_ID;

  can.tryToSend(msg);

  if (DEBUG_LOCAL) {
    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.println("] [ELEICAO TX] Presenca anunciada");
  }
}

void sendLeaderAnnouncement() {
  if (state == STATE_GATEWAY) return;

  CANMessage msg;

  msg.id = CAN_ID_LEADER;
  msg.len = 1;
  msg.data[0] = NODE_ID;

  can.tryToSend(msg);

  if (DEBUG_LOCAL) {
    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.println("] [LIDER TX] Lideranca anunciada");
  }
}

void startElection() {
  if (state == STATE_GATEWAY) {
    Serial.println("[GW] Gateway nao participa da eleicao");
    return;
  }

  if (state == STATE_FAULT || faultDetected) {
    Serial.println("[ELEICAO] Ignorada: no em falha");
    return;
  }

  state = STATE_ELECTION;

  leaderId = 0;
  heartbeatTick = 0;

  joinAccepted = false;
  waitingFirstHeartbeatAfterJoin = false;

  knownNodeCount = 0;
  markNodeObservedFunctional(NODE_ID);

  electionStartTime = millis();
  lastHeartbeatTime = millis();
  lastHeartbeatLedPulse = 0;

  turnAllLedsOff();

  Serial.print("[NODE ");
  Serial.print(NODE_ID);
  Serial.println("] [ELEICAO] Iniciada");

  sendElectionAnnouncement();
}

void finishElection() {
  if (state == STATE_GATEWAY) return;
  if (state != STATE_ELECTION) return;

  uint8_t elected = getHighestKnownNodeId();

  leaderId = elected;

  if (elected == NODE_ID) {
    state = STATE_RECOVERING;
    recoveryStartTime = millis();

    publishStateUpdate(NODE_ID, STATUS_ROLE_LEADER);
    sendStatusRequestBroadcast();

    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.println("] [RECUPERACAO] Eleito lider. Reconstruindo estado");
  } else {
    state = STATE_FOLLOWER;

    publishStateUpdate(NODE_ID, STATUS_FOLLOWER_OK);

    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.print("] [ELEICAO] Lider atual: NODE ");
    Serial.println(leaderId);
  }
}

void finishRecoveryIfNeeded() {
  if (state != STATE_RECOVERING) return;

  if (millis() - recoveryStartTime < RECOVERY_DURATION_MS) {
    return;
  }

  state = STATE_LEADER;

  sendLeaderAnnouncement();
  publishStateUpdate(NODE_ID, STATUS_ROLE_LEADER);

  lastHeartbeatTime = millis();

  Serial.print("[NODE ");
  Serial.print(NODE_ID);
  Serial.println("] [LIDER] Recuperacao concluida. Heartbeat iniciado");
}

/* =========================================================
 * CONFLITO DE LIDER
 * ========================================================= */

void handleLeaderConflict(uint8_t otherLeader) {
  if (state != STATE_LEADER && state != STATE_RECOVERING) return;
  if (otherLeader == NODE_ID) return;

  updateNetworkStatusLocal(NODE_ID, STATUS_LEADER_CONFLICT);
  publishStateUpdate(NODE_ID, STATUS_LEADER_CONFLICT);

  if (otherLeader > NODE_ID) {
    state = STATE_FOLLOWER;
    leaderId = otherLeader;

    publishStateUpdate(NODE_ID, STATUS_FOLLOWER_OK);

    Serial.print("[CONFLITO] Lider com maior ID detectado. Novo lider: NODE ");
    Serial.println(otherLeader);
  } else {
    sendControlFrame(
      CTRL_SUBCMD_ELECTION,
      CTRL_TARGET_BROADCAST,
      CTRL_ACTION_START
    );

    Serial.println("[CONFLITO] Lider com menor ID detectado. Nova eleicao solicitada");
  }
}

/* =========================================================
 * JOIN DINAMICO
 *
 * IMPORTANTE:
 * - Nao existe JOIN automatico durante o boot inicial.
 * - O no em IDLE so tenta JOIN depois de observar heartbeat.
 * - Portanto a rede inicial sempre comeca por 22 00 FF 01.
 * ========================================================= */

void beginJoin(uint8_t observedLeader) {
  if (isGateway()) return;
  if (state != STATE_IDLE) return;
  if (state == STATE_FAULT || faultDetected) return;

  leaderId = observedLeader;
  state = STATE_JOINING;

  joinAttempts = 0;
  joinAccepted = false;
  waitingFirstHeartbeatAfterJoin = false;
  lastJoinRequestTime = 0;

  updateNetworkStatusLocal(NODE_ID, STATUS_JOINING);

  Serial.print("[NODE ");
  Serial.print(NODE_ID);
  Serial.print("] [JOIN] Heartbeat observado. Solicitando entrada ao lider NODE ");
  Serial.println(leaderId);
}

void sendJoinRequest() {
  sendControlFrame(CTRL_SUBCMD_JOIN_REQUEST, NODE_ID, 0x00);

  if (DEBUG_LOCAL) {
    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.print("] [JOIN TX] Solicitacao enviada. Tentativa=");
    Serial.println(joinAttempts);
  }
}

void sendJoinAccept(uint8_t targetId) {
  sendStatusFrame(CTRL_SUBCMD_JOIN_ACCEPT, targetId, leaderId);

  sendHeartbeatRateToTarget(targetId);

  publishStateUpdate(targetId, STATUS_FOLLOWER_OK);

  /*
   * Envia uma pequena carga inicial de estado.
   * Depois disso, a replicacao continua pelo slot logico.
   */
  for (uint8_t i = 0; i < MAX_KNOWN_NODES; i++) {
    if (networkKnown[i]) {
      sendStatusFrame(
        CTRL_SUBCMD_STATE_UPDATE,
        networkNodeIds[i],
        networkNodeStatus[i]
      );
    }
  }

  if (DEBUG_LOCAL) {
    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.print("] [JOIN TX] Entrada aceita para NODE ");
    Serial.println(targetId);
  }
}

void handleJoinRequest(const CANMessage& rx) {
  if (rx.len < 4) return;
  if (rx.data[0] != CTRL_OPCODE) return;
  if (rx.data[1] != CTRL_SUBCMD_JOIN_REQUEST) return;

  uint8_t joiningNode = rx.data[2];

  if (joiningNode == 0) return;

  registerNode(joiningNode);
  updateNetworkStatusLocal(joiningNode, STATUS_JOINING);
  markNodeSeen(joiningNode);

  if (state == STATE_GATEWAY) {
    Serial.print("[GW] JOIN observado de NODE ");
    Serial.println(joiningNode);
    return;
  }

  if (state == STATE_LEADER) {
    sendJoinAccept(joiningNode);
  }
}

void handleJoinAccept(const CANMessage& rx) {
  if (rx.len < 4) return;
  if (rx.data[0] != STATUS_OPCODE) return;
  if (rx.data[1] != CTRL_SUBCMD_JOIN_ACCEPT) return;

  uint8_t targetId = rx.data[2];
  uint8_t acceptedLeader = rx.data[3];

  if (targetId != NODE_ID) return;
  if (state != STATE_JOINING) return;

  leaderId = acceptedLeader;
  joinAccepted = true;
  waitingFirstHeartbeatAfterJoin = true;

  registerNode(NODE_ID);
  updateNetworkStatusLocal(NODE_ID, STATUS_JOINING);

  Serial.print("[NODE ");
  Serial.print(NODE_ID);
  Serial.print("] [JOIN RX] Entrada aceita. Aguardando heartbeat do lider NODE ");
  Serial.println(leaderId);
}

void handleJoinProcess() {
  if (state != STATE_JOINING) return;
  if (joinAccepted) return;

  if (joinAttempts >= JOIN_MAX_ATTEMPTS) {
    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.println("] [JOIN] Sem resposta. Retornando para IDLE");

    state = STATE_IDLE;
    joinAttempts = 0;
    return;
  }

  if (millis() - lastJoinRequestTime >= JOIN_RETRY_MS) {
    lastJoinRequestTime = millis();
    joinAttempts++;

    sendJoinRequest();
  }
}

/* =========================================================
 * HEARTBEAT
 * ========================================================= */

void sendHeartbeat() {
  if (state != STATE_LEADER) return;

  if (isReplicaSyncTick(heartbeatTick)) {
    sendNextReplicaEntry();
  }

  CANMessage msg;

  msg.id = CAN_ID_HEARTBEAT;
  msg.len = 5;

  msg.data[0] = MSG_TYPE_HEARTBEAT;
  msg.data[1] = NODE_ID;
  msg.data[2] = heartbeatTick;
  msg.data[3] = getActiveSensorCount();
  msg.data[4] = heartbeatRateMode;

  can.tryToSend(msg);

  pulseHeartbeatLed();

  if (DEBUG_LOCAL) {
    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.print("] [HEARTBEAT TX] rodada=");
    Serial.print(heartbeatTick);
    Serial.print(" sensores_ativos=");
    Serial.print(msg.data[3]);
    Serial.print(" modo=");
    Serial.print(heartbeatRateMode);
    Serial.print(" periodo=");
    Serial.print(heartbeatPeriodMs);
    Serial.println(" ms");
  }

  heartbeatTick++;
}

/* =========================================================
 * WATCHDOG DE AUSENCIA DOS NOS FUNCIONAIS
 * ========================================================= */

void leaderCheckNodeAbsence() {
  if (state != STATE_LEADER) return;

  unsigned long now = millis();
  unsigned long timeoutMs = nodeAbsenceTimeoutMs();

  for (uint8_t i = 0; i < MAX_KNOWN_NODES; i++) {
    if (!networkKnown[i]) {
      continue;
    }

    uint8_t nodeId = networkNodeIds[i];
    uint8_t status = networkNodeStatus[i];

    if (nodeId == 0) {
      continue;
    }

    if (nodeId == NODE_ID) {
      continue;
    }

    if (status != STATUS_FOLLOWER_OK) {
      continue;
    }

    if (networkLastSeenMs[i] == 0) {
      continue;
    }

    if (now - networkLastSeenMs[i] > timeoutMs) {
      Serial.print("[NODE ");
      Serial.print(NODE_ID);
      Serial.print("] [OMISSAO] NODE ");
      Serial.print(nodeId);
      Serial.println(" ausente por mais de 5 heartbeats. Marcado como FALHA");

      updateNetworkStatusLocal(nodeId, STATUS_FOLLOWER_FAULT);
      publishStateUpdate(nodeId, STATUS_FOLLOWER_FAULT);
    }
  }
}

/* =========================================================
 * GATEWAY
 * ========================================================= */

unsigned long gatewayLeaderTimeoutMs() {
  return heartbeatPeriodMs * GATEWAY_LEADER_TIMEOUT_MULTIPLIER;
}

unsigned long nodeAbsenceTimeoutMs() {
  return heartbeatPeriodMs * NODE_ABSENCE_HEARTBEATS;
}

void gatewayObserveHeartbeat(uint8_t leader,
                             uint8_t tick,
                             uint8_t activeNodes,
                             uint8_t rateMode) {
  bool newLeader = (!gatewayHasLeader || gatewayObservedLeader != leader);

  gatewayHasLeader = true;
  gatewayObservedLeader = leader;
  gatewayLastHeartbeatTick = tick;
  gatewayLastHeartbeatTime = millis();
  gatewayRecoverySent = false;

  if (isValidHeartbeatRate(rateMode) &&
      rateMode != heartbeatRateMode) {
    applyHeartbeatRate(rateMode);
  }

  updateNetworkStatusLocal(leader, STATUS_ROLE_LEADER);

  pulseHeartbeatLed();

  if (newLeader) {
    Serial.print("[GW] Lider observado: NODE ");
    Serial.println(leader);

    gatewayInitialStatusRequested = true;
    gatewayLastStatusRequestTime = millis();
  }

  Serial.print("[GW] HEARTBEAT lider=");
  Serial.print(leader);
  Serial.print(" rodada=");
  Serial.print(tick);
  Serial.print(" sensores_ativos=");
  Serial.print(activeNodes);
  Serial.print(" modo=");
  Serial.print(rateMode);
  Serial.print(" periodo=");
  Serial.print(heartbeatPeriodMs);
  Serial.println(" ms");
}

void gatewayCheckLeaderFailure() {
  if (state != STATE_GATEWAY) return;

  /*
   * Gateway nao inicia rede sozinho.
   * Sem lider conhecido, apenas aguarda comando manual 22 00 FF 01.
   */
  if (!gatewayHasLeader) {
    return;
  }

  if (gatewayRecoverySent) return;

  if (millis() - gatewayLastHeartbeatTime > gatewayLeaderTimeoutMs()) {
    Serial.print("[GW] Falha detectada no lider NODE ");
    Serial.println(gatewayObservedLeader);

    updateNetworkStatusLocal(gatewayObservedLeader, STATUS_FOLLOWER_FAULT);

    Serial.println("[GW] Comando enviado: 22 00 FF 01");

    sendControlFrame(
      CTRL_SUBCMD_ELECTION,
      CTRL_TARGET_BROADCAST,
      CTRL_ACTION_START
    );

    gatewayRecoverySent = true;
    gatewayHasLeader = false;
    gatewayObservedLeader = 0;
  }
}

void gatewayStatusMaintenance() {
  if (state != STATE_GATEWAY) return;

  if (gatewayInitialStatusRequested &&
      millis() - gatewayLastStatusRequestTime > GATEWAY_STATUS_REQUEST_DELAY_MS) {
    sendStatusRequestBroadcast();
    gatewayLastStatusRequestTime = millis();
    gatewayInitialStatusRequested = false;
  }

  if (gatewayHasLeader &&
      millis() - gatewayLastStatusRequestTime > GATEWAY_STATUS_REFRESH_MS) {
    sendStatusRequestBroadcast();
    gatewayLastStatusRequestTime = millis();
  }
}

/* =========================================================
 * SENSOR
 * ========================================================= */

void sendSensorData(uint8_t tick) {
  if (state != STATE_FOLLOWER) return;
  if (state == STATE_FAULT) return;
  if (!sensorEnabled) return;

  uint8_t value = getSimulatedSensorValue();

  CANMessage msg;

  msg.id = CAN_ID_SENSOR;
  msg.len = 5;

  msg.data[0] = NODE_ID;
  msg.data[1] = tick;
  msg.data[2] = value;
  msg.data[3] = sensorEnabled ? 1 : 0;
  msg.data[4] = 0x00;

  can.tryToSend(msg);

  updateNetworkSensorValue(NODE_ID, value, tick);

  if (DEBUG_LOCAL) {
    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.print("] [SENSOR TX] sensor=");
    Serial.print(NODE_ID);
    Serial.print(" rodada=");
    Serial.print(tick);
    Serial.print(" valor=0x");
    Serial.println(value, HEX);
  }
}

void handleSensorMessage(const CANMessage& rx) {
  if (rx.len < 5) return;

  uint8_t fromNode = rx.data[0];
  uint8_t tick     = rx.data[1];
  uint8_t value    = rx.data[2];
  uint8_t enabled  = rx.data[3];

  registerNode(fromNode);
  updateNetworkSensorValue(fromNode, value, tick);

  if (enabled) {
    if (getReplicatedStatus(fromNode) != STATUS_FOLLOWER_FAULT) {
      updateNetworkStatusLocal(fromNode, STATUS_FOLLOWER_OK);
    }
  } else {
    updateNetworkStatusLocal(fromNode, STATUS_FOLLOWER_DISABLED);
  }

  if (state == STATE_GATEWAY) {
    Serial.print("[GW] SENSOR sensor=");
    Serial.print(fromNode);
    Serial.print(" rodada=");
    Serial.print(tick);
    Serial.print(" valor=0x");
    Serial.print(value, HEX);
    Serial.print(" ativo=");
    Serial.println(enabled);
    return;
  }

  if (state == STATE_LEADER) {
    analyzeSensorValue(fromNode, value);
  }
}

/* =========================================================
 * MENSAGENS CAN
 * ========================================================= */

void handleElectionMessage(const CANMessage& rx) {
  if (rx.len < 1) return;

  uint8_t otherNode = rx.data[0];

  if (state == STATE_GATEWAY) {
    registerNode(otherNode);
    updateNetworkStatusLocal(otherNode, STATUS_JOINING);
    markNodeSeen(otherNode);

    Serial.print("[GW] ELEICAO recebida de NODE ");
    Serial.println(otherNode);
    return;
  }

  if (state == STATE_ELECTION) {
    markNodeObservedFunctional(otherNode);
  }
}

void handleLeaderMessage(const CANMessage& rx) {
  if (rx.len < 1) return;

  uint8_t announcedLeader = rx.data[0];

  if ((state == STATE_LEADER || state == STATE_RECOVERING) &&
      announcedLeader != NODE_ID) {
    handleLeaderConflict(announcedLeader);
    return;
  }

  updateNetworkStatusLocal(announcedLeader, STATUS_ROLE_LEADER);
  markNodeSeen(announcedLeader);

  if (state == STATE_GATEWAY) {
    gatewayObservedLeader = announcedLeader;
    gatewayHasLeader = true;
    gatewayLastHeartbeatTime = millis();
    gatewayRecoverySent = false;

    Serial.print("[GW] Lider anunciado: NODE ");
    Serial.println(announcedLeader);
    return;
  }

  if (state == STATE_JOINING) {
    return;
  }

  leaderId = announcedLeader;

  if (announcedLeader == NODE_ID) {
    state = STATE_LEADER;

    if (DEBUG_LOCAL) {
      Serial.print("[NODE ");
      Serial.print(NODE_ID);
      Serial.println("] [LIDER RX] Lideranca confirmada");
    }
  } else if (state != STATE_FAULT) {
    state = STATE_FOLLOWER;

    if (DEBUG_LOCAL) {
      Serial.print("[NODE ");
      Serial.print(NODE_ID);
      Serial.print("] [LIDER RX] Lider atual: NODE ");
      Serial.println(leaderId);
    }
  }
}

void handleHeartbeatMessage(const CANMessage& rx) {
  if (rx.len < 4) return;

  uint8_t msgType      = rx.data[0];
  uint8_t senderLeader = rx.data[1];
  uint8_t tick         = rx.data[2];
  uint8_t activeNodes  = rx.data[3];
  uint8_t rateMode     = heartbeatRateMode;

  if (rx.len >= 5) {
    rateMode = rx.data[4];
  }

  if (msgType != MSG_TYPE_HEARTBEAT) return;

  if ((state == STATE_LEADER || state == STATE_RECOVERING) &&
      senderLeader != NODE_ID) {
    handleLeaderConflict(senderLeader);
    return;
  }

  if (isValidHeartbeatRate(rateMode) &&
      rateMode != heartbeatRateMode) {
    applyHeartbeatRate(rateMode);
  }

  updateNetworkStatusLocal(senderLeader, STATUS_ROLE_LEADER);
  markNodeSeen(senderLeader);

  if (state == STATE_GATEWAY) {
    gatewayObserveHeartbeat(senderLeader, tick, activeNodes, rateMode);
    return;
  }

  /*
   * JOIN dinamico:
   * no em IDLE so tenta JOIN depois que a rede ja possui heartbeat.
   * Isso evita JOIN infinito durante a inicializacao da rede.
   */
  if (state == STATE_IDLE && !faultDetected) {
    beginJoin(senderLeader);
    return;
  }

  if (state == STATE_JOINING &&
      joinAccepted &&
      waitingFirstHeartbeatAfterJoin &&
      senderLeader == leaderId) {
    state = STATE_FOLLOWER;
    waitingFirstHeartbeatAfterJoin = false;

    publishStateUpdate(NODE_ID, STATUS_FOLLOWER_OK);

    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.println("] [JOIN] Primeiro heartbeat recebido. No integrado como FOLLOWER");
    return;
  }

  if (state != STATE_FOLLOWER) return;
  if (senderLeader != leaderId) return;

  pulseHeartbeatLed();

  if (isReplicaSyncTick(tick)) {
    return;
  }

  if (activeNodes == 0) return;
  if (!isNodeActiveForTdma(NODE_ID)) return;

  uint8_t sensorCycle = tick - (tick / REPLICA_SYNC_MODULO);
  uint8_t mySlot = getActiveNodeSlot(NODE_ID);

  if ((sensorCycle % activeNodes) == mySlot) {
    sendSensorData(tick);
  }
}

void handleStateUpdate(const CANMessage& rx) {
  if (rx.len < 4) return;
  if (rx.data[0] != STATUS_OPCODE) return;
  if (rx.data[1] != CTRL_SUBCMD_STATE_UPDATE) return;

  uint8_t nodeId = rx.data[2];
  uint8_t status = rx.data[3];

  if (nodeId != 0) {
    registerNode(nodeId);
  }

  updateNetworkStatusLocal(nodeId, status);
  markNodeSeen(nodeId);

  if (state == STATE_GATEWAY) {
    Serial.print("[GW] Atualizacao de status recebida: ");
    printNetworkStatusLine(nodeId, status);
  }
}

void handleStateForceUpdate(const CANMessage& rx) {
  if (rx.len < 4) return;
  if (rx.data[0] != STATUS_OPCODE) return;
  if (rx.data[1] != CTRL_SUBCMD_STATE_FORCE_UPDATE) return;

  uint8_t nodeId = rx.data[2];
  uint8_t status = rx.data[3];

  if (nodeId != 0) {
    registerNode(nodeId);
  }

  forceNetworkStatusLocal(nodeId, status);
  markNodeSeen(nodeId);

  if (status == STATUS_FOLLOWER_OK) {
    clearFaultRecordForNode(nodeId);
  }

  if (state == STATE_GATEWAY) {
    Serial.print("[GW] Atualizacao FORCADA de status recebida: ");
    printNetworkStatusLine(nodeId, status);
  }
}

void handleControlCanMessage(const CANMessage& rx) {
  if (rx.len < 4) return;

  if (state == STATE_GATEWAY) {
    Serial.print("[GW] CONTROLE RX ");
    Serial.print(rx.data[0], HEX);
    Serial.print(" ");
    Serial.print(rx.data[1], HEX);
    Serial.print(" ");
    Serial.print(rx.data[2], HEX);
    Serial.print(" ");
    Serial.println(rx.data[3], HEX);
  }

  if (rx.data[0] == CTRL_OPCODE &&
      rx.data[1] == CTRL_SUBCMD_JOIN_REQUEST) {
    handleJoinRequest(rx);
    return;
  }

  if (rx.data[0] == STATUS_OPCODE &&
      rx.data[1] == CTRL_SUBCMD_JOIN_ACCEPT) {
    handleJoinAccept(rx);
    return;
  }

  if (rx.data[0] == STATUS_OPCODE &&
      rx.data[1] == CTRL_SUBCMD_STATE_UPDATE) {
    handleStateUpdate(rx);
    return;
  }

  if (rx.data[0] == STATUS_OPCODE &&
      rx.data[1] == CTRL_SUBCMD_STATE_FORCE_UPDATE) {
    handleStateForceUpdate(rx);
    return;
  }

  if (rx.data[0] == CTRL_OPCODE &&
      rx.data[1] == CTRL_SUBCMD_HEARTBEAT_RATE) {
    if (rx.data[2] != CTRL_TARGET_BROADCAST) {
      Serial.println("[CTRL RX] Heartbeat ignorado: alvo deve ser FF");
      return;
    }

    applyHeartbeatRate(rx.data[3]);
    return;
  }

  if (rx.data[0] == CTRL_OPCODE &&
      rx.data[1] == CTRL_SUBCMD_STATUS_REQUEST) {
    handleStatusRequest(rx);
    return;
  }

  if (rx.data[0] == STATUS_OPCODE &&
      rx.data[1] == CTRL_SUBCMD_STATUS_RESPONSE) {
    handleStatusResponse(rx);
    return;
  }

  handleControlMessage(rx);
}

void handleReceivedCanMessage(const CANMessage& rx) {
  if (rx.id == CAN_ID_ELECTION) {
    handleElectionMessage(rx);
  }

  else if (rx.id == CAN_ID_LEADER) {
    handleLeaderMessage(rx);
  }

  else if (rx.id == CAN_ID_HEARTBEAT) {
    handleHeartbeatMessage(rx);
  }

  else if (rx.id == CAN_ID_SENSOR) {
    handleSensorMessage(rx);
  }

  else if (rx.id == CAN_ID_CONTROL) {
    handleControlCanMessage(rx);
  }
}

/* =========================================================
 * SETUP
 * ========================================================= */

void setup() {
  bootTime = millis();

  pinMode(LED_LEADER, OUTPUT);
  pinMode(LED_FAULT, OUTPUT);
  pinMode(LED_STATUS, OUTPUT);

  turnAllLedsOff();

  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("========================================");
  Serial.print("Iniciando NODE ");
  Serial.println(NODE_ID);
  Serial.println("========================================");
  Serial.println("[LEDS] IDLE: todos acesos. Logica invertida: LOW=ligado, HIGH=desligado");

  initNetworkStatusTable();
  initFaultModule();

  applyHeartbeatRate(HB_RATE_NORMAL);

  SPI.begin(13, 19, 23, MCP_CS);

  ACAN2515Settings settings(MCP_QUARTZ_HZ, CAN_BITRATE);
  const uint16_t errorCode = can.begin(settings, [] { can.isr(); });

  if (errorCode != 0) {
    state = STATE_FAULT;
    faultDetected = true;

    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.print("] [ERRO] Falha ao iniciar CAN: 0x");
    Serial.println(errorCode, HEX);

    updateLeds();

    while (true) {
      delay(100);
    }
  }

  if (isGateway()) {
    state = STATE_GATEWAY;

    gatewayHasLeader = false;
    gatewayRecoverySent = false;
    gatewayObservedLeader = 0;
    gatewayLastHeartbeatTime = 0;
    gatewayInitialStatusRequested = false;

    forceNetworkStatusLocal(NODE_ID, STATUS_GATEWAY);

    Serial.println("[GW] Modo gateway ativado");
    Serial.println("[GW] Este no nao participa da eleicao, TDMA ou sensoriamento");
    Serial.println("[GW] Saida serial centralizada neste no");
    Serial.println("[GW] Iniciar eleicao: 22 00 FF 01");
    Serial.println("[GW] Solicitar status: 22 20 FF 00");
    Serial.println("[GW] Velocidade heartbeat: 22 30 FF 01..05");
  } else {
    state = STATE_IDLE;
    forceNetworkStatusLocal(NODE_ID, STATUS_UNKNOWN);

    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.println("] [INIT] Pronto. Aguardando eleicao manual ou heartbeat para JOIN");
  }

  leaderId = 0;
  heartbeatTick = 0;
  faultDetected = false;

  turnAllLedsOff();
}

/* =========================================================
 * LOOP
 * ========================================================= */

void loop() {
  updateLeds();

  handleSerialCommands();

  handleJoinProcess();

  finishRecoveryIfNeeded();

  if (state == STATE_LEADER &&
      millis() - lastHeartbeatTime >= heartbeatPeriodMs) {
    lastHeartbeatTime = millis();
    sendHeartbeat();
  }

  CANMessage rx;

  while (can.receive(rx)) {
    handleReceivedCanMessage(rx);
  }

  if (state == STATE_ELECTION &&
      millis() - electionStartTime >= DISCOVERY_DURATION_MS) {
    finishElection();
  }

  leaderCheckNodeAbsence();

  gatewayCheckLeaderFailure();
  gatewayStatusMaintenance();
}
