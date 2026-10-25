/**
 * @file ioc_panel_canids.h
 * @brief Identificadores e códigos do barramento que o painel interpreta.
 *
 * Cópia dos valores de Codigo/node-can/include/can_ids.h e protocolo.h: o
 * painel e os testes no computador não dependem do firmware do Node. O teste
 * test_ioc_panel.c confere a igualdade com os cabeçalhos originais.
 */
#ifndef IOC_PANEL_CANIDS_H
#define IOC_PANEL_CANIDS_H

#define IOC_ID_ELECTION        0x050
#define IOC_ID_LEADER          0x060
#define IOC_ID_CONTROL         0x080
#define IOC_ID_HEARTBEAT       0x100
#define IOC_ID_SENSOR          0x200
#define IOC_ID_DISCOVERY_BASE  0x280
#define IOC_ID_DISCOVERY_LAST  0x2BF
#define IOC_ID_ASSOC_CMD_FIRST 0x300
#define IOC_ID_ASSOC_CMD_LAST  0x303
#define IOC_ID_DATA_CMD        0x304
#define IOC_ID_TEST_CMD        0x305
#define IOC_ID_TEST_STATUS     0x306
#define IOC_ID_ASSOC_STATUS_BASE 0x320
#define IOC_ID_ASSOC_STATUS_LAST 0x35F
#define IOC_ID_DATA_BASE       0x380
#define IOC_ID_DATA_LAST       0x39F
#define IOC_ID_LOAD_HIGH       0x010
#define IOC_ID_LOAD_LOW        0x7EF

#define IOC_CTRL_OPCODE        0x22
#define IOC_STATUS_OPCODE      0x23
#define IOC_SUB_JOIN_REQUEST   0x40
#define IOC_SUB_STATUS_RESP    0x21
#define IOC_SUB_JOIN_ACCEPT    0x41
#define IOC_SUB_STATE_UPDATE   0x50
#define IOC_SUB_STATE_FORCE    0x51

#define IOC_ST_LEADER          0x00
#define IOC_ST_FOLLOWER_OK     0x01
#define IOC_ST_DISABLED        0x02
#define IOC_ST_FAULT           0x03
#define IOC_ST_GATEWAY         0x04
#define IOC_ST_JOINING         0x05
#define IOC_ST_CONFLICT        0x06

#define IOC_ASSOC_DISCOVERED   0x01
#define IOC_ASSOC_ASSOCIATING  0x02
#define IOC_ASSOC_BOUND        0x03
#define IOC_ASSOC_ONLINE       0x04
#define IOC_ASSOC_STALE        0x05
#define IOC_ASSOC_LOST         0x06
#define IOC_ASSOC_UNBOUND      0x07
#define IOC_ASSOC_REJECTED     0x7F

#define IOC_ASSOC_FAILOVER_OWNER  0x11
#define IOC_ASSOC_FAILOVER_SENSOR 0x12

#define IOC_TEST_STARTED       0x01
#define IOC_TEST_ENDED         0x02

#endif
