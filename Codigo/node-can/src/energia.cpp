#include "energia.h"

#include <Preferences.h>
#include <esp_system.h>
#if IOT_INA219_ADDR >= 0
#include <Wire.h>
#endif
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "node_config.h"

#include "ioc_power.h"
#include "ioc_resetlog.h"

namespace {

constexpr const char* NVS_NAMESPACE = "ioc-diag";
constexpr const char* NVS_KEY = "rst";

uint8_t bootReason = IOC_RST_UNKNOWN;
ioc_rl_counters counters;
ioc_pw power;   // medição da tensão de entrada e da corrente; sem nada ligado, fica parada e muda nada no texto

constexpr bool VIN_ON = IOT_VIN_ADC_PIN >= 0;
constexpr bool INA_ON = IOT_INA219_ADDR >= 0;

#if IOT_INA219_ADDR >= 0
// INA219: registrador de 16 bits, o byte mais significativo primeiro. false se o dispositivo não responde.
bool readIna219(uint8_t reg, uint16_t& value) {
  Wire.beginTransmission(static_cast<uint8_t>(IOT_INA219_ADDR));
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<uint8_t>(IOT_INA219_ADDR), static_cast<uint8_t>(2)) != 2 || Wire.available() < 2) {
    return false;
  }
  const uint8_t high = static_cast<uint8_t>(Wire.read());
  const uint8_t low = static_cast<uint8_t>(Wire.read());
  value = static_cast<uint16_t>(high << 8 | low);
  return true;
}
constexpr uint8_t INA219_REG_SHUNT = 0x01;   // tensão na derivação, passos de 10 uV, com sinal
#endif

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
  char measured[112] = "";   // tensão e corrente, só se ligadas
  ioc_pw_format(&power, measured, sizeof(measured));
  Serial.printf("[NODE %u] [ENERGIA] %s%s wifi_tx_max_dbm=%u.%02u%s\n", NODE_ID, prefix, line,
                static_cast<unsigned>(IOT_WIFI_TX_POWER_QDBM / 4),
                static_cast<unsigned>((IOT_WIFI_TX_POWER_QDBM % 4) * 25), measured);
}

}  // namespace

void energiaInit() {
  ioc_pw_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.vin_enabled = VIN_ON ? 1 : 0;
  cfg.r1_ohm = IOT_VIN_R1_OHM;
  cfg.r2_ohm = IOT_VIN_R2_OHM;
  cfg.warn_mv = IOT_VIN_WARN_MV;
  cfg.crit_mv = IOT_VIN_CRIT_MV;
  cfg.shunt_mohm = INA_ON ? IOT_INA219_SHUNT_MOHM : 0;
  ioc_pw_init(&power, &cfg);
#if IOT_VIN_ADC_PIN >= 0
  pinMode(IOT_VIN_ADC_PIN, INPUT);
  analogSetPinAttenuation(IOT_VIN_ADC_PIN, ADC_11db);   // 0 a cerca de 3,1 V no pino
#endif
#if IOT_INA219_ADDR >= 0
  Wire.begin(IOT_I2C_SDA, IOT_I2C_SCL);
  Wire.setClock(400000);
#endif

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

void energiaPoll() {
  if (!VIN_ON && !INA_ON) return;
  const uint32_t now = millis();
  if (!ioc_pw_due(&power, now)) return;
#if IOT_VIN_ADC_PIN >= 0
  ioc_pw_add_pin_mv(&power, analogReadMilliVolts(IOT_VIN_ADC_PIN));
#endif
#if IOT_INA219_ADDR >= 0
  uint16_t shunt = 0;
  if (readIna219(INA219_REG_SHUNT, shunt)) {
    ioc_pw_add_shunt_reg(&power, shunt);
  } else {
    ioc_pw_current_failed(&power);
  }
#endif
  if (ioc_pw_warning_due(&power, now)) {
    Serial.printf("[NODE %u] [ENERGIA] ATENCAO: tensao de entrada %s, %u mV (aviso abaixo de %u mV): confira fonte, cabos e conexoes\n",
                  NODE_ID, ioc_pw_level_name(ioc_pw_level_now(&power)), static_cast<unsigned>(power.avg_mv),
                  static_cast<unsigned>(IOT_VIN_WARN_MV));
  }
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
    ioc_pw_clear(&power);   // os extremos de tensão e corrente recomeçam junto com a contagem
    const bool saved = saveCounters();
    Serial.printf("[NODE %u] [ENERGIA] contagem zerada%s\n", NODE_ID, saved ? "" : " (a NVS nao foi atualizada)");
    return true;
  }
  Serial.printf("[NODE %u] [ENERGIA] uso: ENERGIA | ENERGIA LIMPAR\n", NODE_ID);
  return true;
}
