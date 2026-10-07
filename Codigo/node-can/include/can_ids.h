#ifndef CAN_IDS_H
#define CAN_IDS_H

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
 * DADOS DOS SENSORES
 * ========================================================= */

#define CAN_ID_SENSOR        0x200

#endif
