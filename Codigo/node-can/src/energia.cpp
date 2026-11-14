#include "energia.h"

#include <Preferences.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "node_config.h"

#include "ioc_resetlog.h"

namespace {

constexpr const char* NVS_NAMESPACE = "ioc-diag";
constexpr const char* NVS_KEY = "rst";

uint8_t bootReason = IOC_RST_UNKNOWN;
ioc_rl_counters counters;

void loadCounters() {
  uint8_t blob[IOC_RL_BYTES];
  size_t len = 0;
  Preferences prefs;
  // Aberto em escrita: no primeiro boot o espaço de nomes ainda não existe.
  if (prefs.begin(NVS_NAMESPACE, false)) {
    len = prefs.getBytes(NVS_KEY, blob, sizeof(blob));
    prefs.end();
  }
  // Registro ausente ou adulterado: recomeça do zero, sem recusar a partida.
  if (len == 0 || !ioc_rl_decode(blob, len, &counters)) memset(&counters, 0, sizeof(counters));
}

bool saveCounters() {
  uint8_t blob[IOC_RL_BYTES];
  const size_t len = ioc_rl_encode(&counters, blob, sizeof(blob));
  bool ok = false;
  Preferences prefs;
  if (len > 0 && prefs.begin(NVS_NAMESPACE, false)) {
    ok = prefs.putBytes(NVS_KEY, blob, len) == len;
    prefs.end();
  }
  return ok;
}

void printState(const char* prefix) {
  char line[128];   // cabe o pior caso: motivo mais longo e três contadores de 10 dígitos
  ioc_rl_format(line, sizeof(line), bootReason, &counters);
  Serial.printf("[NODE %u] [ENERGIA] %s%s wifi_tx_max_dbm=%u.%02u\n", NODE_ID, prefix, line,
                static_cast<unsigned>(IOT_WIFI_TX_POWER_QDBM / 4),
                static_cast<unsigned>((IOT_WIFI_TX_POWER_QDBM % 4) * 25));
}

}  // namespace

void energiaInit() {
  bootReason = static_cast<uint8_t>(esp_reset_reason());
  loadCounters();
  if (!ioc_rl_note(&counters, bootReason)) return;   // partida normal: nada a gravar nem a dizer

  const bool saved = saveCounters();
  printState("ATENCAO: ");
  switch (ioc_rl_classify(bootReason)) {
    case IOC_RL_BROWNOUT:
      Serial.printf("[NODE %u] [ENERGIA] a tensao caiu abaixo do limite do ESP32: confira fonte, cabos e capacitores\n",
                    NODE_ID);
      break;
    case IOC_RL_WATCHDOG:
      Serial.printf("[NODE %u] [ENERGIA] o programa travou e o watchdog reiniciou a placa\n", NODE_ID);
      break;
    default:
      Serial.printf("[NODE %u] [ENERGIA] o programa falhou e a placa reiniciou\n", NODE_ID);
      break;
  }
  if (!saved) Serial.printf("[NODE %u] [ENERGIA] nao foi possivel gravar a contagem na NVS\n", NODE_ID);
}

bool energiaHandleSerialCommand(const char* command) {
  if (strncasecmp(command, "ENERGIA", 7) != 0 || (command[7] != ' ' && command[7] != '\0')) return false;
  const char* argument = command + (command[7] == ' ' ? 8 : 7);
  while (*argument == ' ') ++argument;

  if (*argument == '\0') {
    printState("");
    return true;
  }
  if (strcasecmp(argument, "LIMPAR") == 0) {
    memset(&counters, 0, sizeof(counters));
    const bool saved = saveCounters();
    Serial.printf("[NODE %u] [ENERGIA] contagem zerada%s\n", NODE_ID, saved ? "" : " (a NVS nao foi atualizada)");
    return true;
  }
  Serial.printf("[NODE %u] [ENERGIA] uso: ENERGIA | ENERGIA LIMPAR\n", NODE_ID);
  return true;
}
