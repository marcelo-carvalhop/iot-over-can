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

/* =========================================================
 * MODOS DE LIVENESS/STATUS
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
#define CTRL_SUBCMD_COMMAND_ACK          0x52

#define CTRL_ACK_APPLIED                 0x00
#define CTRL_ACK_REJECTED                0x01

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

/* =========================================================
 * ASSOCIACAO WIRELESS
 * ========================================================= */

#define WIRELESS_ASSOC_ACTION_BIND          0x01
#define WIRELESS_ASSOC_ACTION_UNBIND        0x02
/* Associa e autoriza a reassociação automática (política AUTO). */
#define WIRELESS_ASSOC_ACTION_BIND_AUTO     0x03
/* Altera a política de um vínculo existente. */
#define WIRELESS_ASSOC_ACTION_POLICY_AUTO   0x04
#define WIRELESS_ASSOC_ACTION_POLICY_MANUAL 0x05
/* Enviados pelo líder ao reassociar um sensor (o vínculo nasce com política AUTO). */
#define WIRELESS_ASSOC_ACTION_FAILOVER_OWNER_LOST  0x11
#define WIRELESS_ASSOC_ACTION_FAILOVER_SENSOR_LOST 0x12

#define WIRELESS_ASSOC_DISCOVERED       0x01
#define WIRELESS_ASSOC_ASSOCIATING      0x02
#define WIRELESS_ASSOC_BOUND            0x03
#define WIRELESS_ASSOC_ONLINE           0x04
#define WIRELESS_ASSOC_STALE            0x05
#define WIRELESS_ASSOC_LOST             0x06
#define WIRELESS_ASSOC_UNBOUND          0x07
#define WIRELESS_ASSOC_REJECTED         0x7F
/*
 * No quadro de estado, os dois bits altos do byte de estado indicam a
 * política AUTO (bit 7) e sessão de dados autenticada (bit 6); ver
 * Codigo/common/ioc_link/ioc_failover.h.
 */

/* =========================================================
 * ENSAIOS DE INJECAO DE FALHAS (CAN_ID_TEST_CMD / CAN_ID_TEST_STATUS)
 * ========================================================= */

#define TEST_MAGIC_CMD                  0xA5
#define TEST_MAGIC_STATUS               0x5A

/* Node fora do ar pelo tempo pedido e reinício em seguida (perde o estado em RAM). */
#define TEST_KIND_POWER_CYCLE           0x01
/* Node congelado: não transmite nem processa; retoma com o estado que tinha. */
#define TEST_KIND_SILENCE               0x02
/* Ponto de acesso Wi-Fi do Node desligado pelo tempo pedido. */
#define TEST_KIND_AP_OUTAGE             0x03
/* O Node descarta as sessões autenticadas; os sensores precisam se reautenticar. */
#define TEST_KIND_DROP_SESSION          0x04
/* Carga artificial gerada pela própria Probe 00 (não trafega como comando). */
#define TEST_KIND_BUS_LOAD              0x05
#define TEST_KIND_CANCEL                0x0F

#define TEST_STATE_STARTED              0x01
#define TEST_STATE_ENDED                0x02
#define TEST_STATE_REJECTED             0x03

#define TEST_MAX_DURATION_MS            120000UL

#endif
