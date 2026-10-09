/**
 * @file edge_link.h
 * @brief Material de chave do enlace autenticado com o Node CAN (protocolo v6).
 *
 * As chaves chegam ao firmware em tempo de build (ver CMakeLists.txt e
 * Codigo/scripts/provision_sensor_security.sh). Há dois modos:
 *
 *   DEVICE  o build recebe K_dev e K_net já derivadas para este sensor.
 *           É o modo recomendado: a memória do sensor não contém K_master.
 *   MASTER  o build recebe K_master e o firmware deriva K_dev/K_net no boot.
 *           Serve para bancada, quando o UUID ainda não é conhecido.
 *
 * Sem chave, ou com K_dev provisionada para outro UUID, o enlace permanece
 * desabilitado: o sensor continua anunciando por BLE, mas ignora ofertas e
 * nunca abre sessão (fail-closed).
 */
#ifndef EDGE_LINK_H
#define EDGE_LINK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EDGE_LINK_KEYS_NONE = 0,
    EDGE_LINK_KEYS_DEVICE,
    EDGE_LINK_KEYS_MASTER,
    EDGE_LINK_KEYS_UUID_MISMATCH
} EdgeLinkKeyMode;

void            edge_link_init(uint64_t device_uuid);
bool            edge_link_is_provisioned(void);
EdgeLinkKeyMode edge_link_key_mode(void);
const char     *edge_link_key_mode_name(void);
const uint8_t  *edge_link_device_key(void);
const uint8_t  *edge_link_network_key(void);

#endif /* EDGE_LINK_H */
