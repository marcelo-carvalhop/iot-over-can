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
 * DADOS DOS SENSORES
 * ========================================================= */

#define CAN_ID_SENSOR        0x200

#endif
