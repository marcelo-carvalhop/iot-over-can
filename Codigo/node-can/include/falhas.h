#ifndef FALHAS_H
#define FALHAS_H

#include <Arduino.h>
#include <ACAN2515.h>

/* =========================================================
 * CONFIGURACAO DA DETECCAO DE FALHA REAL
 * ========================================================= */

#define LOCAL_SENSOR_DEMO_VALUE 0xAA
#define SENSOR_NORMAL_VALUE      LOCAL_SENSOR_DEMO_VALUE  // compatibilidade interna
#define SENSOR_FAULT_VALUE     0xFF

#define FAULT_THRESHOLD_COUNT  4
#define MAX_FAULT_NODES        32

/* =========================================================
 * INTERFACES PUBLICAS
 * ========================================================= */

void initFaultModule();

uint8_t getSensorValueForFaultAnalysis(uint8_t measuredValue);

void analyzeSensorValue(uint8_t nodeId, uint8_t value);

uint8_t getLocalStatusCode();

void sendStatusResponse();

void handleStatusRequest(const CANMessage& rx);
void handleStatusResponse(const CANMessage& rx);

void clearFaultRecordForNode(uint8_t nodeId);

#endif
