#include "comandos.h"

#include "can_ids.h"
#include "node_config.h"
#include "node_types.h"
#include "protocolo.h"
#include "falhas.h"

/* =========================================================
 * VARIAVEIS EXTERNAS DO .ino
 * ========================================================= */

extern ACAN2515 can;
extern NodeState state;
extern uint8_t leaderId;
extern bool faultDetected;

extern void startElection();
extern bool isValidHeartbeatRate(uint8_t mode);
extern void applyHeartbeatRate(uint8_t mode);

extern void updateNetworkStatusLocal(uint8_t nodeId, uint8_t status);
extern void publishStateUpdate(uint8_t nodeId, uint8_t status);
extern void forceNetworkStatusLocal(uint8_t nodeId, uint8_t status);
extern void publishStateUpdateForced(uint8_t nodeId, uint8_t status);

/* =========================================================
 * ESTADO LOCAL
 * ========================================================= */

bool sensorEnabled = true;

/* =========================================================
 * AUXILIARES
 * ========================================================= */

static bool canSendAdministrativeCommand() {
  return state == STATE_LEADER || state == STATE_GATEWAY;
}

static void sendControlFrame(uint8_t subcmd,
                             uint8_t targetId,
                             uint8_t action) {
  CANMessage msg;

  msg.id  = CAN_ID_CONTROL;
  msg.len = 4;

  msg.data[0] = CTRL_OPCODE;
  msg.data[1] = subcmd;
  msg.data[2] = targetId;
  msg.data[3] = action;

  can.tryToSend(msg);
}

static void printHexByte(uint8_t value) {
  if (value < 0x10) {
    Serial.print("0");
  }

  Serial.print(value, HEX);
}

/* =========================================================
 * ENVIO SERIAL -> CAN
 * ========================================================= */

static void sendElectionCommand(uint8_t targetId, uint8_t action) {
  if (action != CTRL_ACTION_START) {
    Serial.println("[CTRL] Acao de eleicao invalida");
    return;
  }

  sendControlFrame(CTRL_SUBCMD_ELECTION, targetId, action);

  if (state == STATE_GATEWAY) {
    Serial.println("[GW] Comando enviado: 22 00 FF 01");
  } else {
    Serial.println("[CTRL TX] Comando de eleicao enviado");
  }

  if (targetId == NODE_ID || targetId == CTRL_TARGET_BROADCAST) {
    startElection();
  }
}

static void sendSensorCommand(uint8_t targetId, uint8_t action) {
  if (!canSendAdministrativeCommand()) {
    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.print("] [CTRL] Comando ignorado: este no nao e lider/gateway. Lider atual=");
    Serial.println(leaderId);
    return;
  }

  if (targetId == CTRL_TARGET_BROADCAST) {
    Serial.println("[CTRL] Comando de sensor nao pode usar alvo broadcast");
    return;
  }

  if (action != CTRL_ACTION_OFF &&
      action != CTRL_ACTION_ON &&
      action != CTRL_ACTION_DISABLE_FAULT &&
      action != CTRL_ACTION_CLEAR_FAULT &&
      action != CTRL_ACTION_SIM_FAULT_ON &&
      action != CTRL_ACTION_SIM_FAULT_OFF) {
    Serial.println("[CTRL] Acao de sensor invalida");
    return;
  }

  if (action == CTRL_ACTION_DISABLE_FAULT && state != STATE_LEADER) {
    Serial.println("[CTRL] Desativacao por falha so pode ser enviada pelo lider");
    return;
  }

  sendControlFrame(CTRL_SUBCMD_SENSOR, targetId, action);

  if (state == STATE_GATEWAY) {
    Serial.print("[GW] Comando enviado: 22 10 ");
  } else {
    Serial.print("[CTRL TX] Comando enviado: 22 10 ");
  }

  printHexByte(targetId);
  Serial.print(" ");
  printHexByte(action);
  Serial.println();
}

static void sendStatusRequest(uint8_t targetId, uint8_t action) {
  if (action != 0x00) {
    Serial.println("[CTRL] Requisicao de status deve usar acao 00");
    return;
  }

  sendControlFrame(CTRL_SUBCMD_STATUS_REQUEST, targetId, action);

  if (state == STATE_GATEWAY) {
    Serial.print("[GW] Comando enviado: 22 20 ");
  } else {
    Serial.print("[CTRL TX] Comando enviado: 22 20 ");
  }

  printHexByte(targetId);
  Serial.print(" ");
  printHexByte(action);
  Serial.println();
}

static void sendHeartbeatRateCommand(uint8_t targetId, uint8_t mode) {
  if (!canSendAdministrativeCommand()) {
    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.print("] [CTRL] Comando de heartbeat ignorado: este no nao e lider/gateway. Lider atual=");
    Serial.println(leaderId);
    return;
  }

  if (targetId != CTRL_TARGET_BROADCAST) {
    Serial.println("[CTRL] Frequencia do heartbeat deve ser alterada apenas em broadcast: 22 30 FF modo");
    return;
  }

  if (!isValidHeartbeatRate(mode)) {
    Serial.println("[CTRL] Modo de heartbeat invalido");
    Serial.println("[CTRL] Modos validos: 01, 02, 03, 04, 05");
    return;
  }

  sendControlFrame(CTRL_SUBCMD_HEARTBEAT_RATE, targetId, mode);

  applyHeartbeatRate(mode);

  if (state == STATE_GATEWAY) {
    Serial.print("[GW] Comando enviado: 22 30 ");
  } else {
    Serial.print("[CTRL TX] Comando enviado: 22 30 ");
  }

  printHexByte(targetId);
  Serial.print(" ");
  printHexByte(mode);
  Serial.println();
}

/* =========================================================
 * PARSER SERIAL
 *
 * Formato:
 *   opcode subcmd alvo acao
 *
 * Exemplos:
 *   22 00 FF 01
 *   22 10 02 00
 *   22 10 02 11
 *   22 10 02 44
 *   22 10 02 55
 *   22 10 02 66
 *   22 20 FF 00
 *   22 30 FF 01
 * ========================================================= */

void handleSerialCommands() {
  static char buffer[64];
  static uint8_t idx = 0;

  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      buffer[idx] = '\0';
      idx = 0;

      if (strlen(buffer) == 0) return;

      uint8_t opcode;
      uint8_t subcmd;
      uint8_t targetId;
      uint8_t action;

      int parsed = sscanf(
        buffer,
        "%hhx %hhx %hhx %hhx",
        &opcode,
        &subcmd,
        &targetId,
        &action
      );

      if (parsed != 4) {
        Serial.println("[CTRL] Formato invalido. Use: opcode subcmd alvo acao");
        return;
      }

      if (opcode != CTRL_OPCODE) {
        Serial.println("[CTRL] Opcode invalido");
        return;
      }

      if (subcmd == CTRL_SUBCMD_ELECTION) {
        sendElectionCommand(targetId, action);
      }

      else if (subcmd == CTRL_SUBCMD_SENSOR) {
        sendSensorCommand(targetId, action);
      }

      else if (subcmd == CTRL_SUBCMD_STATUS_REQUEST) {
        sendStatusRequest(targetId, action);
      }

      else if (subcmd == CTRL_SUBCMD_HEARTBEAT_RATE) {
        sendHeartbeatRateCommand(targetId, action);
      }

      else if (subcmd == CTRL_SUBCMD_JOIN_REQUEST) {
        Serial.println("[CTRL] JOIN_REQUEST e automatico; nao envie manualmente pela serial");
      }

      else {
        Serial.println("[CTRL] Subcomando invalido");
      }
    }

    else {
      if (idx < sizeof(buffer) - 1) {
        buffer[idx++] = c;
      }
    }
  }
}

/* =========================================================
 * RECEPCAO CAN -> ACAO LOCAL
 * ========================================================= */

static void handleElectionControl(const CANMessage& rx) {
  uint8_t targetId = rx.data[2];
  uint8_t action   = rx.data[3];

  if (targetId != NODE_ID && targetId != CTRL_TARGET_BROADCAST) {
    return;
  }

  if (action != CTRL_ACTION_START) {
    return;
  }

  if (state == STATE_GATEWAY) {
    Serial.println("[GW] Requisicao de eleicao observada");
    return;
  }

  if (state == STATE_FAULT || faultDetected) {
    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.println("] [ELEICAO] Ignorada: no em falha");
    return;
  }

  Serial.print("[NODE ");
  Serial.print(NODE_ID);
  Serial.println("] [ELEICAO] Comando recebido");

  startElection();
}

static void handleSensorControl(const CANMessage& rx) {
  uint8_t targetId = rx.data[2];
  uint8_t action   = rx.data[3];

  if (targetId != NODE_ID) {
    return;
  }

  if (action == CTRL_ACTION_OFF) {
    if (state == STATE_FAULT || faultDetected) {
      Serial.print("[NODE ");
      Serial.print(NODE_ID);
      Serial.println("] [CTRL RX] OFF ignorado: no esta em FALHA");
      return;
    }

    sensorEnabled = false;

    updateNetworkStatusLocal(NODE_ID, STATUS_FOLLOWER_DISABLED);
    publishStateUpdate(NODE_ID, STATUS_FOLLOWER_DISABLED);

    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.println("] [CTRL RX] Sensor desativado manualmente");
  }

  else if (action == CTRL_ACTION_ON) {
    if (state == STATE_FAULT || faultDetected) {
      Serial.print("[NODE ");
      Serial.print(NODE_ID);
      Serial.println("] [CTRL RX] ON ignorado: use CLEAR_FAULT para limpar falha");
      return;
    }

    sensorEnabled = true;

    updateNetworkStatusLocal(NODE_ID, STATUS_FOLLOWER_OK);
    publishStateUpdate(NODE_ID, STATUS_FOLLOWER_OK);

    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.println("] [CTRL RX] Sensor ativado");
  }

  else if (action == CTRL_ACTION_DISABLE_FAULT) {
    sensorEnabled = false;
    faultDetected = true;
    state = STATE_FAULT;

    updateNetworkStatusLocal(NODE_ID, STATUS_FOLLOWER_FAULT);
    publishStateUpdate(NODE_ID, STATUS_FOLLOWER_FAULT);

    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.println("] [CTRL RX] Sensor desativado por decisao de falha");
  }

  else if (action == CTRL_ACTION_CLEAR_FAULT) {
    faultDetected = false;
    sensorEnabled = true;
    setSimulatedFaultActive(false);

    if (state == STATE_FAULT) {
      state = STATE_FOLLOWER;
    }

    clearFaultRecordForNode(NODE_ID);

    forceNetworkStatusLocal(NODE_ID, STATUS_FOLLOWER_OK);
    publishStateUpdateForced(NODE_ID, STATUS_FOLLOWER_OK);

    Serial.print("[NODE ");
    Serial.print(NODE_ID);
    Serial.println("] [CTRL RX] Falha limpa e sensor reativado");
  }

  else if (action == CTRL_ACTION_SIM_FAULT_ON) {
    if (state == STATE_FAULT || faultDetected) {
      Serial.print("[NODE ");
      Serial.print(NODE_ID);
      Serial.println("] [CTRL RX] Simulacao ignorada: no ja esta em FALHA");
      return;
    }

    setSimulatedFaultActive(true);
  }

  else if (action == CTRL_ACTION_SIM_FAULT_OFF) {
    setSimulatedFaultActive(false);
  }

  else {
    Serial.println("[CTRL RX] Acao de sensor invalida");
  }
}

static void handleHeartbeatRateControl(const CANMessage& rx) {
  uint8_t targetId = rx.data[2];
  uint8_t mode     = rx.data[3];

  if (targetId != CTRL_TARGET_BROADCAST) {
    Serial.println("[CTRL RX] Comando de heartbeat ignorado: alvo deve ser FF");
    return;
  }

  if (!isValidHeartbeatRate(mode)) {
    Serial.println("[CTRL RX] Modo de heartbeat invalido");
    return;
  }

  applyHeartbeatRate(mode);
}

/* =========================================================
 * DISPATCH PRINCIPAL
 * ========================================================= */

void handleControlMessage(const CANMessage& rx) {
  if (rx.id != CAN_ID_CONTROL) return;
  if (rx.len < 4) return;
  if (rx.data[0] != CTRL_OPCODE) return;

  uint8_t subcmd = rx.data[1];

  if (subcmd == CTRL_SUBCMD_ELECTION) {
    handleElectionControl(rx);
  }

  else if (subcmd == CTRL_SUBCMD_SENSOR) {
    handleSensorControl(rx);
  }

  else if (subcmd == CTRL_SUBCMD_STATUS_REQUEST) {
    return;
  }

  else if (subcmd == CTRL_SUBCMD_HEARTBEAT_RATE) {
    handleHeartbeatRateControl(rx);
  }

  else if (subcmd == CTRL_SUBCMD_JOIN_REQUEST) {
    return;
  }

  else {
    Serial.println("[CTRL RX] Subcomando invalido");
  }
}
