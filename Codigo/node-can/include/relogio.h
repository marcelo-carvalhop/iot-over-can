#ifndef RELOGIO_H
#define RELOGIO_H

#include <Arduino.h>
#include <ACAN2515.h>
#include <stdint.h>

/*
 * Relógio da rede (formato em Codigo/common/ioc_link/ioc_time.h).
 *
 * Cada Node e a Probe 00 mantêm a hora UTC sobre o millis(). A hora chega do
 * terminal (comando TIME na serial da Probe 00, enviado pela TUI ao conectar
 * e a cada 10 minutos), de um relógio de tempo real ou de outro Node. Quem
 * não tem hora pede ao barramento na partida e quando um painel a pede;
 * respondem a Probe 00 e o líder.
 */

/* A cada laço; answersRequests é verdadeiro no líder. A Probe 00 sempre responde. */
void relogioPoll(bool answersRequests);
bool relogioHandleCanMessage(const CANMessage& rx);
/* TIME <ms UTC desde 1970> ajusta e divulga a hora do terminal; TIME sozinho mostra o estado. */
bool relogioHandleSerialCommand(const char* command);

bool relogioValid();
int64_t relogioNowMs();
/* Quadro de hora do próprio Node (IOC_TIME_LEN bytes); false se não há hora. */
bool relogioOwnFrame(uint8_t* out);
/* Pede a hora ao barramento (no máximo um pedido a cada 5 s). */
void relogioRequest();

#endif
