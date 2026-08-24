#include "falhas.h"

#include "can_ids.h"
#include "node_config.h"
#include "node_types.h"
#include "protocolo.h"
#include "comandos.h"

/* =========================================================
 * VARIAVEIS DEFINIDAS NO ARQUIVO PRINCIPAL .ino
 * ========================================================= */

extern ACAN2515 can;
extern NodeState state;
extern uint8_t leaderId;
extern bool faultDetected;

extern void updateNetworkStatusLocal(uint8_t nodeId, uint8_t status);
extern void publishStateUpdate(uint8_t nodeId, uint8_t status);
extern void forceNetworkStatusLocal(uint8_t nodeId, uint8_t status);
extern void publishStateUpdateForced(uint8_t nodeId, uint8_t status);

/* =========================================================
 * SIMULACAO LOCAL DE FALHA
 * ========================================================= */

static bool simulatedFaultActive = false;

/* =========================================================
 * TABELA DE CONTROLE DE FALHAS OBSERVADAS PELO LIDER
 * ========================================================= */

static uint8_t faultNodeIds[MAX_FAULT_NODES];
static uint8_t faultCounters[MAX_FAULT_NODES];
static bool faultConfirmed[MAX_FAULT_NODES];

static uint8_t faultNodeCount = 0;

/* =========================================================
 * FUNCOES INTERNAS
 * ========================================================= */

static int findFaultIndex(uint8_t nodeId) {
  for (uint8_t i = 0; i < faultNodeCount; i++) {
    if (faultNodeIds[i] == nodeId) {
      return i;
    }
  }

  return -1;
}

static int registerFaultNode(uint8_t nodeId) {
  int existing = findFaultIndex(nodeId);

  if (existing >= 0) {
    return existing;
  }

  if (faultNodeCount >= MAX_FAULT_NODES) {
    return -1;
  }

  faultNodeIds[faultNodeCount] = nodeId;
  faultCounters[faultNodeCount] = 0;
  faultConfirmed[faultNodeCount] = false;

  faultNodeCount++;

  return faultNodeCount - 1;
}

void clearFaultRecordForNode(uint8_t nodeId) {
  int idx = findFaultIndex(nodeId);

  if (idx < 0) {
    return;
  }

  faultCounters[idx] = 0;
  faultConfirmed[idx] = false;
}

static void sendFaultDisableCommand(uint8_t targetId) {
  CANMessage msg;

  msg.id = CAN_ID_CONTROL;
  msg.len = 4;

  msg.data[0] = CTRL_OPCODE;
  msg.data[1] = CTRL_SUBCMD_SENSOR;
  msg.data[2] = targetId;
  msg.data[3] = CTRL_ACTION_DISABLE_FAULT;

  can.tryToSend(msg);

  Serial.print("[NODE ");
  Serial.print(NODE_ID);
  Serial.print("] [FALHA TX] Desativacao por falha enviada para NODE ");
  Serial.println(targetId);
}

/* =========================================================
 * INICIALIZACAO
 * ========================================================= */

void initFaultModule() {
  simulatedFaultActive = false;
  faultNodeCount = 0;

  Serial.print("[NODE ");
  Serial.print(NODE_ID);
  Serial.println("] [FALHA] Simulacao por software inicializada");
}

/* =========================================================
 * SIMULACAO LOCAL DA MEDICAO
 * ========================================================= */

void setSimulatedFaultActive(bool active) {
  simulatedFaultActive = active;

  Serial.print("[NODE ");
  Serial.print(NODE_ID);
  Serial.print("] [FALHA] Simulacao de valor absurdo ");
  Serial.println(active ? "ATIVADA" : "DESATIVADA");
}

bool isSimulatedFaultActive() {
  return simulatedFaultActive;
}

uint8_t getSimulatedSensorValue() {
  if (simulatedFaultActive) {
    return SENSOR_FAULT_VALUE;
  }

  return SENSOR_NORMAL_VALUE;
}

/* =========================================================
 * ANALISE DE FALHA PELO LIDER
 * ========================================================= */

void analyzeSensorValue(uint8_t nodeId, uint8_t value) {
  if (state != STATE_LEADER) return;
  if (nodeId == NODE_ID) return;

  int idx = registerFaultNode(nodeId);

  if (idx < 0) {
    Serial.println("[FALHA] Tabela de falhas cheia");
    return;
  }

  if (faultConfirmed[idx]) {
    return;
  }

  if (value == SENSOR_FAULT_VALUE) {
    faultCounters[idx]++;

    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.print("] [FALHA] Valor divergente do NODE ");
    Serial.print(nodeId);
    Serial.print(" contador=");
    Serial.println(faultCounters[idx]);

    if (faultCounters[idx] >= FAULT_THRESHOLD_COUNT) {
      faultConfirmed[idx] = true;

      Serial.print("[NODE ");
      Serial.print(NODE_ID);
      Serial.print("] [FALHA] NODE ");
      Serial.print(nodeId);
      Serial.println(" confirmado como defeituoso");

      updateNetworkStatusLocal(nodeId, STATUS_FOLLOWER_FAULT);
      publishStateUpdate(nodeId, STATUS_FOLLOWER_FAULT);

      sendFaultDisableCommand(nodeId);
    }
  }

  else {
    faultCounters[idx] = 0;

    /*
     * A transicao FAULT -> OK e protegida em updateNetworkStatusLocal().
     * Portanto isto nao limpa uma falha confirmada.
     */
    updateNetworkStatusLocal(nodeId, STATUS_FOLLOWER_OK);
  }
}

/* =========================================================
 * STATUS LOCAL
 * ========================================================= */

uint8_t getLocalStatusCode() {
  if (state == STATE_GATEWAY) {
    return STATUS_GATEWAY;
  }

  if (state == STATE_JOINING) {
    return STATUS_JOINING;
  }

  if (state == STATE_LEADER || state == STATE_RECOVERING) {
    return STATUS_ROLE_LEADER;
  }

  if (state == STATE_FAULT || faultDetected) {
    return STATUS_FOLLOWER_FAULT;
  }

  if (!sensorEnabled) {
    return STATUS_FOLLOWER_DISABLED;
  }

  return STATUS_FOLLOWER_OK;
}

/* =========================================================
 * RESPOSTA DE STATUS
 *
 * Formato:
 *   23 21 ID cod_status
 * ========================================================= */

void sendStatusResponse() {
  CANMessage msg;

  msg.id = CAN_ID_CONTROL;
  msg.len = 4;

  msg.data[0] = STATUS_OPCODE;
  msg.data[1] = CTRL_SUBCMD_STATUS_RESPONSE;
  msg.data[2] = NODE_ID;
  msg.data[3] = getLocalStatusCode();

  can.tryToSend(msg);

  updateNetworkStatusLocal(NODE_ID, msg.data[3]);

  Serial.print("[NODE ");
  Serial.print(NODE_ID);
  Serial.print("] [STATUS TX] codigo=");
  Serial.println(msg.data[3], HEX);
}

/* =========================================================
 * REQUISICAO DE STATUS
 *
 * Esperado:
 *   22 20 FF 00
 * ========================================================= */

void handleStatusRequest(const CANMessage& rx) {
  if (rx.id != CAN_ID_CONTROL) return;
  if (rx.len < 4) return;

  if (rx.data[0] != CTRL_OPCODE) return;
  if (rx.data[1] != CTRL_SUBCMD_STATUS_REQUEST) return;

  uint8_t targetId = rx.data[2];

  if (targetId != NODE_ID && targetId != CTRL_TARGET_BROADCAST) {
    return;
  }

  sendStatusResponse();
}

/* =========================================================
 * RECEPCAO DE RESPOSTAS DE STATUS
 * ========================================================= */

void handleStatusResponse(const CANMessage& rx) {
  if (rx.id != CAN_ID_CONTROL) return;
  if (rx.len < 4) return;

  if (rx.data[0] != STATUS_OPCODE) return;
  if (rx.data[1] != CTRL_SUBCMD_STATUS_RESPONSE) return;

  uint8_t nodeId = rx.data[2];
  uint8_t status = rx.data[3];

  updateNetworkStatusLocal(nodeId, status);

  Serial.print("[NODE ");
  Serial.print(NODE_ID);
  Serial.print("] [STATUS RX] NODE ");
  Serial.print(nodeId);
  Serial.print(" estado=");

  if (status == STATUS_ROLE_LEADER) {
    Serial.println("LIDER");
  }

  else if (status == STATUS_FOLLOWER_OK) {
    Serial.println("ATIVO");
  }

  else if (status == STATUS_FOLLOWER_DISABLED) {
    Serial.println("DESATIVADO");
  }

  else if (status == STATUS_FOLLOWER_FAULT) {
    Serial.println("FALHA");
  }

  else if (status == STATUS_GATEWAY) {
    Serial.println("GATEWAY");
  }

  else if (status == STATUS_JOINING) {
    Serial.println("ENTRANDO");
  }

  else if (status == STATUS_LEADER_CONFLICT) {
    Serial.println("CONFLITO_LIDER");
  }

  else if (status == STATUS_UNKNOWN) {
    Serial.println("DESCONHECIDO");
  }

  else {
    Serial.println("INVALIDO");
  }
}
