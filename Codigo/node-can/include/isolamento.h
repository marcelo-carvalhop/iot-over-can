#ifndef ISOLAMENTO_H
#define ISOLAMENTO_H

#include <stdint.h>

/*
 * Isolamento do barramento (ioc_isolation.h): com o cabo CAN solto o módulo segue ligado, com o ponto de
 * acesso no ar e o sensor em sessão, e os dados do sensor não chegam a lugar nenhum. O módulo se
 * considera isolado quando fica IOT_ISOLATION_MS sem receber quadro algum e o controlador diz que os
 * quadros dele não são confirmados; então libera os sensores (CMD_RELEASE), desliga ponto de acesso e
 * ofertas, e o líder pode reassociá-los a outro módulo. Volta quando os quadros chegam sem interrupção
 * por IOT_ISOLATION_RECOVER_MS.
 *
 * Isolado, o seguidor também não pede eleição (main.ino): o pedido não chegaria a ninguém, o módulo se
 * elegeria sozinho e, na volta do cabo, os pedidos que ficaram na fila do controlador derrubariam o líder
 * da rede inteira.
 *
 * Comando serial:  ISOLAMENTO   mostra o estado (barramento, silêncio, contadores do controlador).
 */

#ifndef IOT_ISOLATION_MS
#define IOT_ISOLATION_MS 6000        /* 0 desliga a detecção */
#endif
#ifndef IOT_ISOLATION_RECOVER_MS
#define IOT_ISOLATION_RECOVER_MS 3000
#endif

void isolamentoInit();
void isolamentoOnReceive();   /* a cada quadro recebido do barramento */
/* A cada passagem do laço; consulta o controlador 10 vezes por segundo. force confere já (o seguidor que está
   prestes a pedir eleição quer saber se perdeu o barramento antes de pedir). */
void isolamentoPoll(bool force = false);
bool isolamentoActive();
bool isolamentoHandleSerialCommand(const char* command);

#endif
