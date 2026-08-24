#ifndef PROTOCOLO_H
#define PROTOCOLO_H

/* =========================================================
 * TIPOS DE MENSAGEM
 * ========================================================= */

#define MSG_TYPE_HEARTBEAT           0x01

/* =========================================================
 * OPCODE PRINCIPAL DE CONTROLE
 * ========================================================= */

#define CTRL_OPCODE                  0x22

/* =========================================================
 * SUBCOMANDOS DE CONTROLE
 * ========================================================= */

#define CTRL_SUBCMD_ELECTION         0x00
#define CTRL_SUBCMD_SENSOR           0x10
#define CTRL_SUBCMD_STATUS_REQUEST   0x20
#define CTRL_SUBCMD_HEARTBEAT_RATE   0x30
#define CTRL_SUBCMD_JOIN_REQUEST     0x40

/* =========================================================
 * DESTINOS ESPECIAIS
 * ========================================================= */

#define CTRL_TARGET_BROADCAST        0xFF

/* =========================================================
 * ACOES GENERICAS
 * ========================================================= */

#define CTRL_ACTION_START            0x01

/* =========================================================
 * ACOES DE SENSOR
 * ========================================================= */

#define CTRL_ACTION_OFF              0x00
#define CTRL_ACTION_ON               0x11
#define CTRL_ACTION_DISABLE_FAULT    0x33
#define CTRL_ACTION_CLEAR_FAULT      0x44

/*
 * Simulacao de falha por software.
 * Substitui a antiga simulacao por GPIO.
 */
#define CTRL_ACTION_SIM_FAULT_ON     0x55
#define CTRL_ACTION_SIM_FAULT_OFF    0x66

/* =========================================================
 * MODOS DE HEARTBEAT
 *
 * 01 = 2000 ms
 * 02 = 1500 ms
 * 03 = 1000 ms
 * 04 = 750 ms
 * 05 = 500 ms
 * ========================================================= */

#define HB_RATE_VERY_SLOW            0x01
#define HB_RATE_SLOW                 0x02
#define HB_RATE_NORMAL               0x03
#define HB_RATE_FAST                 0x04
#define HB_RATE_VERY_FAST            0x05

/* =========================================================
 * OPCODE DE STATUS
 * ========================================================= */

#define STATUS_OPCODE                0x23

/* =========================================================
 * SUBCOMANDOS DE STATUS
 * ========================================================= */

#define CTRL_SUBCMD_STATUS_RESPONSE      0x21
#define CTRL_SUBCMD_JOIN_ACCEPT          0x41
#define CTRL_SUBCMD_STATE_UPDATE         0x50
#define CTRL_SUBCMD_STATE_FORCE_UPDATE   0x51

/* =========================================================
 * CODIGOS DE STATUS DOS NOS
 * ========================================================= */

#define STATUS_ROLE_LEADER           0x00
#define STATUS_FOLLOWER_OK           0x01
#define STATUS_FOLLOWER_DISABLED     0x02
#define STATUS_FOLLOWER_FAULT        0x03
#define STATUS_GATEWAY               0x04
#define STATUS_JOINING               0x05
#define STATUS_LEADER_CONFLICT       0x06
#define STATUS_UNKNOWN               0xFF

#endif
