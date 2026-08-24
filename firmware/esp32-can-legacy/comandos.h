#ifndef COMANDOS_H
#define COMANDOS_H

#include <Arduino.h>
#include <ACAN2515.h>

/* =========================================================
 * ESTADO DO SENSOR
 * ========================================================= */

extern bool sensorEnabled;

/* =========================================================
 * INTERFACES PUBLICAS
 * ========================================================= */

void handleSerialCommands();
void handleControlMessage(const CANMessage& rx);

#endif
