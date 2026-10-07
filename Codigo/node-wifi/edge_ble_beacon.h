#ifndef EDGE_BLE_BEACON_H
#define EDGE_BLE_BEACON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Chamado para cada Manufacturer Specific Data recebido durante a varredura
 * de ofertas. Executa no contexto da pilha Bluetooth; deve ser curto.
 */
typedef void (*edge_ble_mfg_handler_t)(const uint8_t *mfg, size_t len);

bool edge_ble_beacon_init(void);
bool edge_ble_beacon_is_active(void);

/**
 * Liga ou desliga a varredura passiva usada para receber a oferta de vínculo
 * anunciada pelo Node CAN responsável. Deve ser chamada do laço principal.
 */
void edge_ble_offer_scan_set_handler(edge_ble_mfg_handler_t handler);
void edge_ble_offer_scan_enable(bool enable);
bool edge_ble_offer_scan_is_active(void);

#endif
