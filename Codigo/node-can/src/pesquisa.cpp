#include "pesquisa.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "can_ids.h"
#include "fault_injection.h"
#include "node_config.h"

#include "ioc_bench.h"

#include "../../node-wifi/edge_protocol_definitions.h"

extern unsigned long heartbeatPeriodMs;

#ifndef IOT_FOLLOWER_LEADER_WATCHDOG
#define IOT_FOLLOWER_LEADER_WATCHDOG 1
#endif

namespace {

uint64_t benchClock() { return pesquisaMonoUs(); }

void benchEmit(const char* line, void*) {
  // Na Probe a linha segue o padrão das demais ([GW]); nos Nodes, o do monitor.
  if (NODE_ID == 0) Serial.printf("[GW] %s\n", line);
  else Serial.printf("[NODE %u] %s\n", static_cast<unsigned>(NODE_ID), line);
}

}  // namespace

void pesquisaPrintConfig() {
  // Parâmetros desta compilação que entram na análise; o registro do
  // experimento os copia, e o hash da configuração os inclui.
  Serial.printf("[GW] CONFIG node=%u bitrate=%lu hb_period_ms=%lu assoc_stale_ms=%lu assoc_lost_ms=%lu "
                "assoc_hysteresis_ms=%lu stream_period_ms=%lu ping_ms=%lu session_timeout_ms=%lu "
                "offer_hold_ms=%lu load_ids=0x%03X,0x%03X,0x%03X fault_injection=%u follower_watchdog=%u t_us=%llu\n",
                static_cast<unsigned>(NODE_ID), static_cast<unsigned long>(CAN_NOMINAL_BITRATE),
                static_cast<unsigned long>(heartbeatPeriodMs), static_cast<unsigned long>(IOT_ASSOC_STALE_MS),
                static_cast<unsigned long>(IOT_ASSOC_LOST_MS), static_cast<unsigned long>(IOT_ASSOC_HYSTERESIS_MS),
                static_cast<unsigned long>(IOT_STREAM_PERIOD_MS), static_cast<unsigned long>(NET_LIVENESS_INTERVAL_MS),
                static_cast<unsigned long>(NET_SESSION_TIMEOUT_MS), static_cast<unsigned long>(NET_OFFER_SWITCH_HOLD_MS),
                static_cast<unsigned>(CAN_ID_TEST_LOAD_HIGH), static_cast<unsigned>(CAN_ID_TEST_LOAD_MID),
                static_cast<unsigned>(CAN_ID_TEST_LOAD_LOW), static_cast<unsigned>(IOT_FAULT_INJECTION),
                static_cast<unsigned>(IOT_FOLLOWER_LEADER_WATCHDOG),
                static_cast<unsigned long long>(pesquisaMonoUs()));
}

bool pesquisaHandleSerialCommand(const char* command) {
  if (!command) return false;
  if (strcasecmp(command, "CONFIG") == 0) {
    pesquisaPrintConfig();
    return true;
  }
  if (strncasecmp(command, "BENCH", 5) != 0 || (command[5] != ' ' && command[5] != '\0')) return false;
  const char* rest = command + 5;
  while (*rest == ' ') ++rest;
  if (strncasecmp(rest, "CRYPTO", 6) != 0) {
    Serial.println("[GW] BENCH_ERROR use: BENCH CRYPTO [n]");
    return true;
  }
  rest += 6;
  const unsigned long n = strtoul(rest, nullptr, 10);
  // O laço principal fica parado durante a medição (algumas dezenas de ms):
  // use com a rede em repouso, não durante um ensaio.
  const int ops = ioc_bench_crypto(NODE_ID == 0 ? "probe" : "node",
                                   static_cast<uint16_t>(n > IOC_BENCH_MAX_N ? IOC_BENCH_MAX_N : n),
                                   benchClock, benchEmit, nullptr);
  Serial.printf("%sBENCH_DONE ops=%d cpu_mhz=%u t_us=%llu\n", NODE_ID == 0 ? "[GW] " : "", ops,
                static_cast<unsigned>(getCpuFrequencyMhz()), static_cast<unsigned long long>(pesquisaMonoUs()));
  return true;
}
