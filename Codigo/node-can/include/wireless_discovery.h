#ifndef WIRELESS_DISCOVERY_H
#define WIRELESS_DISCOVERY_H

#include <Arduino.h>
#include <ACAN2515.h>

void wirelessDiscoveryInit();
void wirelessDiscoveryPoll();
bool wirelessDiscoveryHandleCanMessage(const CANMessage& rx);

// Comandos textuais aceitos apenas pela Probe 00:
//   WIRELESS BIND <node> <uuid> [AUTO]
//   WIRELESS UNBIND <node> <uuid>
//   WIRELESS POLICY <node> <uuid> AUTO|MANUAL
// AUTO autoriza o líder a reassociar o sensor a outro Node quando o
// responsável sair do ar ou deixar de alcançá-lo.
bool wirelessAssociationHandleSerialCommand(const char* command);

// O líder declarou o Node ausente: os sensores dele com política AUTO podem
// ser reassociados sem esperar o silêncio do vínculo.
void wirelessFailoverOnNodeFault(uint8_t nodeId);

// Este Node ficou sem executar o laço: a visão que ele tem dos vínculos e das
// observações está desatualizada e não pode servir de base para decisões.
void wirelessFailoverOnLocalStall();

#endif
