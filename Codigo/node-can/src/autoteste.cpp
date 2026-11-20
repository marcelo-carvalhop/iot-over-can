#include "autoteste.h"

#include <Arduino.h>
#include <ACAN2515.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "node_config.h"
#include "node_types.h"

#include "ioc_selftest.h"

extern ACAN2515 can;
extern NodeState state;
extern void updateLeds();
extern void selftestLeds(uint8_t mask);   // main.ino: bit 0 líder, bit 1 estado, bit 2 falha

namespace {

constexpr uint32_t LOOPBACK_WAIT_MS = 20;     // o quadro volta em microssegundos; isto é só folga
constexpr uint32_t SETTLE_MS = 5;             // dá tempo de esvaziar o que já estava na fila de envio
constexpr uint8_t MODE_ATTEMPTS = 3;
constexpr uint32_t MODE_RETRY_MS = 2;
constexpr uint32_t RESTORE_WINDOW_MS = 200;
constexpr uint32_t LED_STEP_MS = 250;
// A biblioteca ACAN2515 devolve um código diferente de zero quando o controlador não confirma o modo
// pedido no prazo; qualquer valor diferente de zero é falha.
constexpr uint8_t PART_CAN = 1;
constexpr uint8_t PART_LEDS = 2;

struct Tally {
  uint8_t checks = 0;
  uint8_t warn = 0;
  uint8_t fail = 0;

  void add(ioc_st_level level) {
    ++checks;
    if (level == IOC_ST_WARN) ++warn;
    if (level == IOC_ST_FAIL) ++fail;
  }
  ioc_st_level worst() const { return fail ? IOC_ST_FAIL : (warn ? IOC_ST_WARN : IOC_ST_PASS); }
};

const char* stateLabel(NodeState value) {
  switch (value) {
    case STATE_IDLE: return "IDLE";
    case STATE_GATEWAY: return "GATEWAY";
    case STATE_JOINING: return "JOINING";
    case STATE_ELECTION: return "ELECTION";
    case STATE_RECOVERING: return "RECOVERING";
    case STATE_LEADER: return "LEADER";
    case STATE_FOLLOWER: return "FOLLOWER";
    case STATE_FAULT: return "FAULT";
  }
  return "?";
}

bool requestMode(ACAN2515Settings::RequestedMode mode) {
  for (uint8_t attempt = 0; attempt < MODE_ATTEMPTS; ++attempt) {
    if (can.changeModeOnTheFly(mode) == 0) return true;
    delay(MODE_RETRY_MS);
  }
  return false;
}

// O MCP2515 só troca de modo quando o barramento está livre; o pedido continua valendo no controlador
// mesmo quando a biblioteca desiste cedo. Como no ensaio de silêncio (fault_injection.cpp), o retorno ao
// modo normal é repetido por até RESTORE_WINDOW_MS: um módulo não pode ficar mudo depois do autoteste.
bool restoreNormalMode() {
  const uint32_t start = millis();
  do {
    if (can.changeModeOnTheFly(ACAN2515Settings::NormalMode) == 0) return true;
    delay(MODE_RETRY_MS);
  } while (millis() - start < RESTORE_WINDOW_MS);
  return false;
}

void printHint(const char* hint) {
  if (hint[0] != '\0') Serial.printf("[NODE %u] [SELFTEST] DICA: %s\n", static_cast<unsigned>(NODE_ID), hint);
}

ioc_st_level checkCanErrors() {
  const uint8_t rec = can.receiveErrorCounter();
  const uint8_t tec = can.transmitErrorCounter();
  const uint8_t eflg = can.errorFlagRegister();
  const char* reason = "OK";
  const ioc_st_level level = ioc_st_classify_errors(rec, tec, eflg, &reason);
  Serial.printf("[NODE %u] [SELFTEST] CHECK=CAN_ERRORS RESULT=%s REASON=%s REC=%u TEC=%u EFLG=0x%02X\n",
                static_cast<unsigned>(NODE_ID), ioc_st_level_name(level), reason, static_cast<unsigned>(rec),
                static_cast<unsigned>(tec), static_cast<unsigned>(eflg));
  printHint(ioc_st_error_hint(reason));
  return level;
}

// Espera o quadro voltar pelo laço interno; false se nada chegou no prazo.
bool waitLoopbackFrame(CANMessage& rx) {
  const uint32_t start = millis();
  do {
    if (can.receive(rx)) return true;
    delay(1);
  } while (millis() - start < LOOPBACK_WAIT_MS);
  return false;
}

ioc_st_level checkCanLoopback() {
  const unsigned node = static_cast<unsigned>(NODE_ID);

  if (!requestMode(ACAN2515Settings::LoopBackMode)) {
    Serial.printf("[NODE %u] [SELFTEST] CHECK=CAN_LOOPBACK RESULT=FAIL REASON=MODO_LACO FRAMES=0/%u\n", node,
                  static_cast<unsigned>(IOC_ST_FRAMES));
    printHint("o controlador nao entrou no modo de laco interno: confira a alimentacao e o cristal do modulo MCP2515");
    if (!restoreNormalMode()) {
      Serial.printf("[NODE %u] [SELFTEST] CHECK=CAN_RESTORE RESULT=FAIL REASON=MODO_NORMAL\n", node);
      printHint("o controlador nao voltou ao modo normal: reinicie a placa antes de usar o barramento");
    }
    return IOC_ST_FAIL;
  }

  // O que já estava na fila de recepção, ou ainda saía da fila de envio, não faz parte do teste.
  delay(SETTLE_MS);
  CANMessage stale;
  while (can.receive(stale)) {
  }

  uint8_t passed = 0;
  ioc_st_lb_result worst = IOC_ST_LB_OK;
  for (uint8_t n = 0; n < IOC_ST_FRAMES; ++n) {
    ioc_st_frame sent;
    ioc_st_frame_at(n, &sent);
    CANMessage tx;
    tx.id = sent.id;
    tx.ext = false;
    tx.rtr = false;
    tx.len = sent.len;
    memcpy(tx.data, sent.data, sent.len);

    ioc_st_lb_result result = IOC_ST_LB_NO_FRAME;
    if (can.tryToSend(tx)) {
      CANMessage rx;
      if (waitLoopbackFrame(rx)) {
        ioc_st_frame got;
        got.id = rx.id;
        got.len = rx.len;
        memset(got.data, 0, sizeof(got.data));
        memcpy(got.data, rx.data, rx.len > sizeof(got.data) ? sizeof(got.data) : rx.len);
        result = ioc_st_loopback_compare(&sent, &got);
      }
    }
    if (result == IOC_ST_LB_OK) {
      ++passed;
    } else if (worst == IOC_ST_LB_OK) {
      worst = result;   // o primeiro defeito é o que explica os demais
    }
  }

  const bool restored = restoreNormalMode();
  const ioc_st_level level = (worst == IOC_ST_LB_OK) ? IOC_ST_PASS : IOC_ST_FAIL;
  Serial.printf("[NODE %u] [SELFTEST] CHECK=CAN_LOOPBACK RESULT=%s REASON=%s FRAMES=%u/%u\n", node,
                ioc_st_level_name(level), ioc_st_lb_text(worst), static_cast<unsigned>(passed),
                static_cast<unsigned>(IOC_ST_FRAMES));
  printHint(ioc_st_lb_hint(worst));
  if (!restored) {
    Serial.printf("[NODE %u] [SELFTEST] CHECK=CAN_RESTORE RESULT=FAIL REASON=MODO_NORMAL\n", node);
    printHint("o controlador nao voltou ao modo normal: reinicie a placa antes de usar o barramento");
    return IOC_ST_FAIL;
  }
  return level;
}

// Os LEDs só se confirmam a olho: um por vez, depois os três juntos. O resultado do teste de LEDs não
// entra na contagem do veredito, que é o que o programa consegue conferir sozinho.
void checkLeds() {
  static const uint8_t SEQUENCE[] = {0x01, 0x02, 0x04, 0x07};
  for (uint8_t mask : SEQUENCE) {
    selftestLeds(mask);
    delay(LED_STEP_MS);
  }
  updateLeds();   // devolve os LEDs ao que o estado do módulo manda
  Serial.printf("[NODE %u] [SELFTEST] CHECK=LEDS RESULT=VISUAL SEQUENCE=LEADER,STATUS,FAULT,ALL\n",
                static_cast<unsigned>(NODE_ID));
  Serial.printf("[NODE %u] [SELFTEST] DICA: confirme a olho que os tres LEDs acenderam um por vez e depois juntos\n",
                static_cast<unsigned>(NODE_ID));
}

void run(uint8_t parts) {
  const unsigned node = static_cast<unsigned>(NODE_ID);
  const uint32_t start = millis();
  Tally tally;

  Serial.printf("[NODE %u] [SELFTEST] INICIO PARTES=%s%s%s ESTADO=%s\n", node, (parts & PART_CAN) ? "CAN" : "",
                (parts == (PART_CAN | PART_LEDS)) ? "," : "", (parts & PART_LEDS) ? "LEDS" : "", stateLabel(state));

  if (parts & PART_CAN) {
    tally.add(checkCanErrors());
    tally.add(checkCanLoopback());
  }
  if (parts & PART_LEDS) checkLeds();

  // Sem nenhuma verificação automática (só os LEDs), o programa não tem veredito: quem decide é o olho.
  const char* verdict = tally.checks > 0 ? ioc_st_level_name(tally.worst()) : "VISUAL";
  Serial.printf("[NODE %u] [SELFTEST] RESULT=%s CHECKS=%u WARN=%u FAIL=%u DURATION_MS=%lu\n", node,
                verdict, static_cast<unsigned>(tally.checks),
                static_cast<unsigned>(tally.warn), static_cast<unsigned>(tally.fail),
                static_cast<unsigned long>(millis() - start));
}

}  // namespace

bool autotesteHandleSerialCommand(const char* command) {
  if (strncasecmp(command, "SELFTEST", 8) != 0 || (command[8] != ' ' && command[8] != '\0')) return false;
  const char* argument = command + (command[8] == ' ' ? 9 : 8);
  while (*argument == ' ') ++argument;

  uint8_t parts = PART_CAN | PART_LEDS;
  if (strcasecmp(argument, "CAN") == 0) {
    parts = PART_CAN;
  } else if (strcasecmp(argument, "LEDS") == 0) {
    parts = PART_LEDS;
  } else if (*argument != '\0') {
    Serial.printf("[NODE %u] [SELFTEST] uso: SELFTEST | SELFTEST CAN | SELFTEST LEDS\n", static_cast<unsigned>(NODE_ID));
    return true;
  }

  if (state != STATE_IDLE && state != STATE_GATEWAY) {
    Serial.printf("[NODE %u] [SELFTEST] RECUSADO ESTADO=%s: o modulo participa da rede e nao pode ficar mudo; "
                  "rode o autoteste antes de ligar o barramento\n",
                  static_cast<unsigned>(NODE_ID), stateLabel(state));
    return true;
  }
  run(parts);
  return true;
}
