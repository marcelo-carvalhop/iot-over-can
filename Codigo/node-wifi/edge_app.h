/**
 * @file edge_app.h
 * @brief Ganchos que cada firmware de sensor implementa para o enlace compartilhado.
 *
 * O driver de rede (edge_network_driver.c), o anúncio BLE (edge_ble_beacon.c)
 * e o diagnóstico (edge_diagnostics.c) são os mesmos no sensor de vibração
 * (MPU6050, Codigo/node-wifi) e no sensor acústico (INMP441,
 * Codigo/node-audio). O que depende do sensor fica atrás destas funções,
 * resolvidas na ligação: cada firmware compila exatamente uma implementação.
 *
 *   vibração   edge_app_vibration.c
 *   acústico   Codigo/node-audio/audio_app.c
 *
 * A validação de Payload_Configuration segue o mesmo princípio com
 * edge_config_validate() (config_validation.h): config_validation.c no
 * sensor de vibração, audio_config.c no acústico.
 */
#ifndef EDGE_APP_H
#define EDGE_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge_protocol_definitions.h"

/** NodeProfileID anunciado no BLE e no HELLO autenticado. */
uint8_t  edge_app_profile_id(void);

/** Preenche os campos de configuração de Payload_ConfigMenu (faixas e valores atuais). */
void     edge_app_fill_config_menu(Payload_ConfigMenu *menu);

/** Valores efetivos ecoados em Payload_ConfigAck. */
uint16_t edge_app_effective_window_size(void);
float    edge_app_effective_sample_rate_hz(void);

/**
 * Configuração recebida do Node e já aprovada por edge_config_validate().
 * Executa no contexto do lwIP: apenas registra o pedido; o laço principal o
 * aplica e responde com edge_net_send_config_ack().
 */
void     edge_app_queue_config(const Payload_Configuration *cfg);

/**
 * Comando de sessão próprio do sensor, já autenticado e livre de repetição.
 * Executa no contexto do lwIP: apenas registra o pedido. Retorna false para
 * um comando que o sensor não conhece (o driver o conta como recusa de
 * protocolo).
 */
bool     edge_app_handle_session_command(uint8_t cmd_type, const uint8_t *inner, size_t len);

#endif /* EDGE_APP_H */
