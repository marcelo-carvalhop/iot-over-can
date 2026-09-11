/**
 * @file edge_network_driver.h
 * @brief Camada Wi-Fi/UDP de sessão com o nó CAN pai; compatibilidade transitória até o vínculo BLE.
 * Atende ao Item 12: Arquitetura de Rede Híbrida.
 */
#ifndef EDGE_NETWORK_DRIVER_H
#define EDGE_NETWORK_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "edge_protocol_definitions.h"
#include "dsp_pipeline.h"

typedef enum {
    NET_STATE_DISABLED  = 0,
    NET_STATE_DISCOVERY = 1,
    NET_STATE_BOUND     = 2
} NetworkMode;

void        edge_net_init(void);
bool        edge_net_set_enabled(bool enabled);
bool        edge_net_is_enabled(void);
bool        edge_ble_beacon_is_active(void);
bool        edge_net_provision_wifi(const char *ssid, const char *password);
void        edge_net_clear_wifi_provisioning(void);
bool        edge_net_is_wifi_provisioned(void);
uint32_t    edge_net_next_discovery_delay_ms(void);
NetworkMode edge_net_get_state(void);
void        edge_net_poll_timeout(void);
void        edge_net_send_discovery_beacon(uint8_t bite_status);
void        edge_net_send_telemetry(const DSP_AnalysisResult *dsp_res, OperationModeFSM mode, 
                                    uint16_t dtc_code, const float *fft_mag, uint16_t fft_bins);
void        edge_net_send_urgent_dtc(const DTC_Record *dtc_rec);
void        edge_net_send_config_ack(const Payload_Configuration *cfg, uint8_t status);
void        edge_net_send_dtc_snapshot(void);

#endif // EDGE_NETWORK_DRIVER_H