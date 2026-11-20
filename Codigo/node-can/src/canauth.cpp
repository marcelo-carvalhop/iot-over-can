#include "canauth.h"

#if IOT_CAN_AUTH

#include <Arduino.h>
#include <Preferences.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "can_ids.h"
#include "node_config.h"

#include "ioc_canauth.h"
#include "ioc_link.h"
#include "ioc_secpdu.h"
#include "ioc_sha256.h"

#ifndef IOT_LINK_MASTER_KEY_HEX
#define IOT_LINK_MASTER_KEY_HEX ""
#endif

static_assert(sizeof(IOT_LINK_MASTER_KEY_HEX) == 2 * IOC_LINK_KEY_LEN + 1,
              "IOT_CAN_AUTH=1 exige IOT_LINK_MASTER_KEY_HEX (64 caracteres hexadecimais) em todos os modulos e na Probe 00");
static_assert(CAN_ID_AUTH_TAG == IOC_CA_TAG_ID, "identificador da etiqueta diverge de ioc_canauth.h");
static_assert(CAN_ID_WIRELESS_DATA_CMD == IOC_CA_ID_DATA_CMD, "0x304 diverge de ioc_canauth.h");
static_assert(CAN_ID_TEST_CMD == IOC_CA_ID_TEST_CMD, "0x305 diverge de ioc_canauth.h");

namespace {

constexpr const char* NVS_NAMESPACE = "ioc-canauth";
constexpr const char* NVS_KEY = "fresh";

bool ready = false;          // chave válida: sem ela a Probe não etiqueta e os módulos recusam tudo
ioc_ca_tx tx;                // Probe
ioc_ca_rx rx;                // módulos
uint64_t savedValue = 0;     // o que está na NVS: teto na Probe, último aceito nos módulos
uint32_t tagsSent = 0;
uint32_t tagsLost = 0;
uint32_t refusedNoKey = 0;

// Limite de taxa do aviso: um ataque a todo quadro não pode inundar a serial.
bool loggedOnce = false;
uint32_t lastLogMs = 0;
uint32_t omitted = 0;

uint64_t loadValue() {
  uint8_t blob[8] = {0};
  size_t len = 0;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {   // em escrita: no primeiro boot o espaço de nomes ainda não existe
    len = prefs.getBytes(NVS_KEY, blob, sizeof(blob));
    prefs.end();
  }
  if (len != sizeof(blob)) return 0;
  uint64_t value = 0;
  for (uint8_t b : blob) value = (value << 8) | b;
  return value;
}

bool saveValue(uint64_t value) {
  uint8_t blob[8];
  for (int i = 0; i < 8; ++i) blob[i] = static_cast<uint8_t>(value >> (56 - 8 * i));
  bool ok = false;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    ok = prefs.putBytes(NVS_KEY, blob, sizeof(blob)) == sizeof(blob);
    prefs.end();
  }
  if (ok) savedValue = value;
  return ok;
}

void report(ioc_ca_why why, uint32_t id) {
  const uint32_t now = millis();
  if (loggedOnce && now - lastLogMs < 1000) {
    ++omitted;
    return;
  }
  Serial.printf("[NODE %u] [CANAUTH] descartado id=0x%03lX motivo=%s omitidos=%lu aceitos=%lu invalidos=%lu "
                "sem_etiqueta=%lu orfas=%lu\n",
                static_cast<unsigned>(NODE_ID), static_cast<unsigned long>(id), ioc_ca_why_name(why),
                static_cast<unsigned long>(omitted), static_cast<unsigned long>(rx.accepted),
                static_cast<unsigned long>(rx.bad_tag), static_cast<unsigned long>(rx.no_tag),
                static_cast<unsigned long>(rx.orphan_tag));
  loggedOnce = true;
  lastLogMs = now;
  omitted = 0;
}

}  // namespace

void canauthInit() {
  uint8_t master[IOC_LINK_KEY_LEN];
  if (!ioc_link_hex_to_key(IOT_LINK_MASTER_KEY_HEX, master) || ioc_link_key_is_zero(master)) {
    ready = false;
    Serial.printf("[NODE %u] [CANAUTH] ERRO: chave do enlace invalida; %s\n", static_cast<unsigned>(NODE_ID),
                  NODE_ID == 0 ? "a Probe nao etiqueta os comandos e os modulos vao recusa-los"
                               : "este modulo recusa os comandos 0x304 e 0x305");
    return;
  }
  uint8_t bus[IOC_LINK_KEY_LEN];
  ioc_secpdu_derive_bus_key(master, bus);
  ioc_secure_zero(master, sizeof(master));

  const uint64_t stored = loadValue();
  if (NODE_ID == 0) {
    ioc_ca_tx_init(&tx, bus, stored);
    saveValue(ioc_ca_tx_boot_ceiling(&tx));   // nenhum valor desta partida pode reaparecer depois de uma queda de energia
  } else {
    ioc_ca_rx_init(&rx, bus, stored);
    savedValue = stored;
  }
  ioc_secure_zero(bus, sizeof(bus));
  ready = true;
  Serial.printf("[NODE %u] [CANAUTH] LIGADO papel=%s atualidade=%llu\n", static_cast<unsigned>(NODE_ID),
                NODE_ID == 0 ? "PROBE" : "MODULO", static_cast<unsigned long long>(stored));
}

void canauthPoll() {
  if (NODE_ID == 0 || !ready) return;
  const uint16_t expired = ioc_ca_rx_expire(&rx, millis());
  if (expired) report(IOC_CA_WHY_NO_TAG, expired);
}

bool canauthTagFor(const CANMessage& sent, CANMessage& tag) {
  if (NODE_ID != 0 || ioc_ca_kind_of(static_cast<uint16_t>(sent.id)) < 0) return false;
  if (!ready) return false;
  if (ioc_ca_tx_needs_save(&tx, savedValue)) saveValue(ioc_ca_tx_next_ceiling(&tx));   // grava antes de usar o valor
  tag.id = CAN_ID_AUTH_TAG;
  tag.len = IOC_CA_TAG_LEN;
  if (!ioc_ca_tx_make_tag(&tx, static_cast<uint16_t>(sent.id), sent.data, sent.len, tag.data)) return false;
  ++tagsSent;
  return true;
}

void canauthTagNotSent() { ++tagsLost; }

CanAuthVerdict canauthOnReceive(const CANMessage& msg, CANMessage& released) {
  if (NODE_ID == 0) return CANAUTH_PASS;   // a Probe emite os comandos; não há o que verificar nela
  if (ioc_ca_kind_of(static_cast<uint16_t>(msg.id)) < 0 && msg.id != CAN_ID_AUTH_TAG) return CANAUTH_PASS;
  if (!ready) {   // sem chave não há como conferir: recusa, em vez de obedecer a qualquer um
    ++refusedNoKey;
    return CANAUTH_CONSUMED;
  }

  ioc_ca_frame out;
  ioc_ca_why why = IOC_CA_WHY_NONE;
  switch (ioc_ca_rx_frame(&rx, millis(), static_cast<uint16_t>(msg.id), msg.data, msg.len, &out, &why)) {
    case IOC_CA_PASS:
      return CANAUTH_PASS;
    case IOC_CA_HOLD:
      if (why != IOC_CA_WHY_NONE) report(why, msg.id);
      return CANAUTH_CONSUMED;
    case IOC_CA_RELEASE:
      if (ioc_ca_rx_needs_save(&rx, savedValue)) saveValue(rx.freshness);
      released.id = out.id;
      released.len = out.len;
      memcpy(released.data, out.data, out.len);
      return CANAUTH_RELEASE;
    default:
      // Etiqueta que não confere: o descartado é o comando que ela acompanhava (o tipo vem no primeiro byte dela).
      report(why, why == IOC_CA_WHY_BAD_TAG ? (msg.data[0] == 0 ? IOC_CA_ID_DATA_CMD : IOC_CA_ID_TEST_CMD) : msg.id);
      return CANAUTH_CONSUMED;
  }
}

bool canauthHandleSerialCommand(const char* command) {
  if (strncasecmp(command, "CANAUTH", 7) != 0 || (command[7] != '\0' && command[7] != ' ')) return false;
  const char* arg = command + 7;
  while (*arg == ' ') ++arg;

  if (*arg == '\0') {
    if (NODE_ID == 0) {
      Serial.printf("[NODE 0] [CANAUTH] estado=%s papel=PROBE atualidade=%llu gravada=%llu etiquetas=%lu perdidas=%lu\n",
                    ready ? "LIGADO" : "SEM_CHAVE", static_cast<unsigned long long>(tx.freshness),
                    static_cast<unsigned long long>(savedValue), static_cast<unsigned long>(tagsSent),
                    static_cast<unsigned long>(tagsLost));
    } else {
      Serial.printf("[NODE %u] [CANAUTH] estado=%s papel=MODULO atualidade=%llu gravada=%llu aceitos=%lu invalidos=%lu "
                    "sem_etiqueta=%lu orfas=%lu recusados_sem_chave=%lu\n",
                    static_cast<unsigned>(NODE_ID), ready ? "LIGADO" : "SEM_CHAVE",
                    static_cast<unsigned long long>(rx.freshness), static_cast<unsigned long long>(savedValue),
                    static_cast<unsigned long>(rx.accepted), static_cast<unsigned long>(rx.bad_tag),
                    static_cast<unsigned long>(rx.no_tag), static_cast<unsigned long>(rx.orphan_tag),
                    static_cast<unsigned long>(refusedNoKey));
    }
    return true;
  }

  if (strncasecmp(arg, "SYNC", 4) == 0 && (arg[4] == ' ' || arg[4] == '\0')) {
    if (NODE_ID == 0) {
      Serial.println("[NODE 0] [CANAUTH] SYNC so vale nos modulos: a Probe mostra a atualidade dela em CANAUTH");
      return true;
    }
    const char* number = arg + 4;
    while (*number == ' ') ++number;
    char* end = nullptr;
    const unsigned long long value = strtoull(number, &end, 10);
    if (*number == '\0' || *number < '0' || *number > '9' || (end && *end != '\0')) {
      Serial.printf("[NODE %u] [CANAUTH] uso: CANAUTH SYNC <numero>\n", static_cast<unsigned>(NODE_ID));
      return true;
    }
    if (!ready) {
      Serial.printf("[NODE %u] [CANAUTH] SYNC recusado: chave do enlace invalida\n", static_cast<unsigned>(NODE_ID));
      return true;
    }
    rx.freshness = value;
    const bool saved = saveValue(value);
    Serial.printf("[NODE %u] [CANAUTH] atualidade acertada em %llu%s\n", static_cast<unsigned>(NODE_ID), value,
                  saved ? "" : " (a gravacao na NVS falhou; vale ate o proximo reinicio)");
    return true;
  }

  Serial.printf("[NODE %u] [CANAUTH] uso: CANAUTH | CANAUTH SYNC <numero>\n", static_cast<unsigned>(NODE_ID));
  return true;
}

#endif  // IOT_CAN_AUTH
