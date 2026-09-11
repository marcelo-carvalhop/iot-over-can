\
#ifndef WIRELESS_DISCOVERY_H
#define WIRELESS_DISCOVERY_H

#include <Arduino.h>
#include <ACAN2515.h>

void wirelessDiscoveryInit();
void wirelessDiscoveryPoll();
bool wirelessDiscoveryHandleCanMessage(const CANMessage& rx);

#endif
