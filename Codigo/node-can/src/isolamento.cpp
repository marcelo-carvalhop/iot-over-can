#include "isolamento.h"

#include <Arduino.h>
#include <ACAN2515.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "node_config.h"
#include "wireless_link.h"

#include "ioc_isolation.h"

extern ACAN2515 can;

namespace {

constexpr uint32_t CHECK_PERIOD_MS = 100;   // o controlador é lido pelo SPI: 10 vezes por segundo bastam
constexpr uint32_t ISOLATION_MS = IOT_ISOLATION_MS;
constexpr uint32_t RECOVER_MS = IOT_ISOLATION_RECOVER_MS;

// Função e não comparação direta: com IOT_ISOLATION_MS=0 (detecção desligada) "x >= 0" seria um erro de compilação
// com -Werror=type-limits.
bool elapsedAtLeast(uint32_t sinceMs, uint32_t limitMs) {
  return sinceMs >= limitMs;
}

ioc_iso iso;
uint32_t lastCheckMs = 0;
uint32_t isolatedSinceMs = 0;

}  // namespace

void isolamentoInit() {
  ioc_iso_init(&iso, millis());
  lastCheckMs = millis();
}

void isolamentoOnReceive() {
  ioc_iso_note_rx(&iso, millis());
}

bool isolamentoActive() {
  return iso.isolated != 0;
}

void isolamentoPoll(bool force) {
  // A Probe 00 não tem sensor para liberar; módulo e Probe se mantêm iguais no resto.
  if (NODE_ID == 0) return;
  const uint32_t now = millis();
  if (!force && now - lastCheckMs < CHECK_PERIOD_MS) return;
  lastCheckMs = now;

  // O controlador só é consultado quando a decisão depende dele: no silêncio, ou já isolado.
  const bool quiet = elapsedAtLeast(now - iso.last_rx_ms, ISOLATION_MS);
  uint8_t tec = 0;
  uint8_t eflg = 0;
  if (quiet || iso.isolated) {
    tec = can.transmitErrorCounter();
    eflg = can.errorFlagRegister();
  }
  const ioc_iso_event event = ioc_iso_poll(&iso, now, tec, eflg, ISOLATION_MS, RECOVER_MS);
  if (event == IOC_ISO_ENTERED) {
    isolatedSinceMs = now;
    Serial.printf("[NODE %u] [ISOLADO] sem quadros do barramento ha %lu ms e transmissoes sem confirmacao "
                  "(TEC=%u EFLG=0x%02X): sensores liberados, ponto de acesso desligado\n",
                  static_cast<unsigned>(NODE_ID), static_cast<unsigned long>(now - iso.last_rx_ms),
                  static_cast<unsigned>(tec), static_cast<unsigned>(eflg));
    wirelessLinkSetIsolated(true);
  } else if (event == IOC_ISO_EXITED) {
    Serial.printf("[NODE %u] [ISOLADO] barramento de volta apos %lu ms: os vinculos que o lider nao moveu "
                  "voltam a ser oferecidos\n",
                  static_cast<unsigned>(NODE_ID), static_cast<unsigned long>(now - isolatedSinceMs));
    wirelessLinkSetIsolated(false);
  }
}

bool isolamentoHandleSerialCommand(const char* command) {
  if (strncasecmp(command, "ISOLAMENTO", 10) != 0 || command[10] != '\0') return false;
  const uint32_t now = millis();
  Serial.printf("[NODE %u] [ISOLADO] estado=%s detecao_ms=%lu silencio_ms=%lu TEC=%u REC=%u EFLG=0x%02X\n",
                static_cast<unsigned>(NODE_ID), iso.isolated ? "ISOLADO" : "LIGADO",
                static_cast<unsigned long>(ISOLATION_MS), static_cast<unsigned long>(now - iso.last_rx_ms),
                static_cast<unsigned>(can.transmitErrorCounter()), static_cast<unsigned>(can.receiveErrorCounter()),
                static_cast<unsigned>(can.errorFlagRegister()));
  return true;
}
