#ifndef WIRELESS_LINK_H
#define WIRELESS_LINK_H

#include <Arduino.h>
#include <ACAN2515.h>

/*
 * Plano de dados dos sensores wireless.
 *
 * Node funcional (NODE_ID > 0)
 *   - anuncia por BLE a oferta de vínculo autenticada para cada sensor
 *     associado e mantém um ponto de acesso Wi-Fi enquanto houver vínculo;
 *   - autentica o sensor por desafio-resposta (HMAC-SHA256) e mantém uma
 *     sessão com contador anti-replay;
 *   - encaminha telemetria, espectro, DTCs e confirmações ao CAN;
 *   - executa os comandos de dados recebidos pelo CAN.
 *
 * Probe 00 (NODE_ID == 0)
 *   - converte "CMD TARGET=NN.CC ACTION=..." em quadros de comando;
 *   - remonta as transferências dos Nodes e emite as linhas TEL, FRAG, DTC,
 *     ACK e WIRELESS_LINK/WIRELESS_CONFIG para a TUI.
 */

void wirelessLinkInit();
void wirelessLinkPoll();
bool wirelessLinkHandleCanMessage(const CANMessage& rx);
bool wirelessLinkHandleSerialCommand(const char* command);

/* Chamados por wireless_discovery.cpp quando a tabela de vínculos muda. */
void wirelessLinkOnBound(uint64_t uuid, uint8_t childId);
void wirelessLinkOnUnbound(uint64_t uuid, uint8_t childId);

/* true quando o Node possui a chave de enlace e pode oferecer o plano de dados. */
bool wirelessLinkHasKey();

/* true quando há sessão de dados autenticada com o sensor. */
bool wirelessLinkIsSecure(uint64_t uuid);

/*
 * Ganchos dos ensaios de injeção de falhas (fault_injection.cpp).
 *   DropSessions  descarta as sessões; os sensores precisam se reautenticar.
 *   ApOutage      desliga o ponto de acesso por durationMs (0 = religa agora).
 *   RadioOff      desliga ponto de acesso e anúncio até o reinício do Node.
 */
void wirelessLinkInjectDropSessions();
void wirelessLinkInjectApOutage(uint32_t durationMs);
void wirelessLinkRadioOff();

#endif
