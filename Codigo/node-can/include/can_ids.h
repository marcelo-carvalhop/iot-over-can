#ifndef CAN_IDS_H
#define CAN_IDS_H

/* Taxa nominal do barramento (CAN clássico). */
#define CAN_NOMINAL_BITRATE  500000UL

/* =========================================================
 * MENSAGENS DE ELEICAO
 * ========================================================= */

#define CAN_ID_ELECTION      0x050
#define CAN_ID_LEADER        0x060

/* =========================================================
 * MENSAGENS DE CONTROLE E STATUS
 * ========================================================= */

#define CAN_ID_CONTROL       0x080

/* =========================================================
 * LIVENESS LEASE E SUPERVISAO
 * ========================================================= */

#define CAN_ID_HEARTBEAT     0x100


/* =========================================================
 * DESCOBERTA WIRELESS DISTRIBUIDA (CAN CLASSICO)
 *
 * Cada Node funcional usa dois IDs consecutivos:
 *   BASE + NODE_ID*2     -> parte A
 *   BASE + NODE_ID*2 + 1 -> parte B
 * NODE_ID 1..31 => 0x282..0x2BF. Probe 00 apenas observa.
 * ========================================================= */
#define CAN_ID_WIRELESS_DISCOVERY_BASE  0x280
#define CAN_ID_WIRELESS_DISCOVERY_LAST  0x2BF

/* =========================================================
 * ASSOCIACAO WIRELESS
 *
 * Probe 00 envia o pedido em dois frames fixos. Cada Node
 * publica o estado da associacao em dois IDs exclusivos.
 * ========================================================= */
#define CAN_ID_WIRELESS_ASSOC_CMD_A       0x300
#define CAN_ID_WIRELESS_ASSOC_CMD_B       0x301
/*
 * Mesmo formato, para os pedidos emitidos pelo lider (reassociacao
 * automatica). Identificadores proprios porque um identificador CAN deve ter
 * um unico transmissor: 0x300/0x301 pertencem a Probe 00.
 */
#define CAN_ID_WIRELESS_ASSOC_LEADER_A    0x302
#define CAN_ID_WIRELESS_ASSOC_LEADER_B    0x303
#define CAN_ID_WIRELESS_ASSOC_STATUS_BASE 0x320
#define CAN_ID_WIRELESS_ASSOC_STATUS_LAST 0x35F

/* =========================================================
 * PLANO DE DADOS WIRELESS
 *
 * Comando (Probe 00 -> Node): um quadro
 *   seq | node | filho | opcode | arg0..arg3
 *
 * Dados (Node -> barramento): transferencias segmentadas em um
 * identificador exclusivo por Node, BASE + NODE_ID (0x381..0x39F).
 * Sao os identificadores de menor prioridade do projeto: trafego
 * de sensores nunca vence a arbitragem contra eleicao, controle,
 * liveness ou associacao. Formato em Codigo/common/ioc_link/ioc_wdata.h.
 * ========================================================= */
#define CAN_ID_WIRELESS_DATA_CMD          0x304
#define CAN_ID_WIRELESS_DATA_BASE         0x380
#define CAN_ID_WIRELESS_DATA_LAST         0x39F

/* =========================================================
 * ENSAIOS DE INJECAO DE FALHAS
 *
 * Comando (Probe 00 -> Node):
 *   seq | alvo | tipo | arg0 | duracao (2 bytes, unidades de 100 ms) | arg1 | 0xA5
 * Estado (Node -> barramento):
 *   seq | node | tipo | estado | duracao (2 bytes) | 0 | 0x5A
 *
 * Os dois identificadores abaixo sao usados apenas pela Probe 00 para gerar
 * carga artificial: um vence a arbitragem contra todo o trafego do projeto,
 * o outro perde para todo ele. Nenhum Node os interpreta. (0x7EF e o maior
 * identificador de 11 bits permitido de uso geral: os sete bits mais
 * significativos nao podem ser todos recessivos.)
 * ========================================================= */
#define CAN_ID_TEST_CMD                   0x305
#define CAN_ID_TEST_STATUS                0x306
#define CAN_ID_TEST_LOAD_HIGH             0x010
#define CAN_ID_TEST_LOAD_LOW              0x7EF

/* =========================================================
 * DADOS DOS SENSORES
 * ========================================================= */

#define CAN_ID_SENSOR        0x200

#endif
