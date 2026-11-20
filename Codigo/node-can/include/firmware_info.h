#ifndef FIRMWARE_INFO_H
#define FIRMWARE_INFO_H

/*
 * Identificação do firmware do Módulo CAN e da Probe 00: versão e commit vêm do
 * build (arquivo VERSION e git; ver ioc_version.h) e saem no boot, em
 * PROBE_VERSION (Probe 00) e em VERSION (módulos). Os recursos anunciados ficam
 * num só lugar para que a linha de boot e o comando não divirjam.
 */
#include "ioc_version.h"

#define IOT_FEATURES "WIRELESS_DATA,ACOUSTIC_DATA,BUS_METRICS,FAULT_INJECTION,FAILOVER,RESEARCH,TIME_SYNC"
#define IOT_PROTOCOL_NAME "CAN_CLASSIC_V1"

#endif
