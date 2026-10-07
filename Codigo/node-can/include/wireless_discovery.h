#ifndef WIRELESS_DISCOVERY_H
#define WIRELESS_DISCOVERY_H

#include <Arduino.h>
#include <ACAN2515.h>

void wirelessDiscoveryInit();
void wirelessDiscoveryPoll();
bool wirelessDiscoveryHandleCanMessage(const CANMessage& rx);

// Comandos textuais aceitos apenas pela Probe 00:
//   WIRELESS BIND <node> <uuid>
//   WIRELESS UNBIND <node> <uuid>
bool wirelessAssociationHandleSerialCommand(const char* command);

#endif
