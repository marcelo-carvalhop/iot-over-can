/**
 * @file edge_app_vibration.h
 * @brief Parte do enlace específica do sensor de vibração (MPU6050).
 *
 * Implementa os ganchos de edge_app.h e monta a telemetria de vibração
 * (Payload_TelemetryStream). Antes da v0.19 este código ficava dentro de
 * edge_network_driver.c, que passou a ser compartilhado com o sensor acústico.
 */
#ifndef EDGE_APP_VIBRATION_H
#define EDGE_APP_VIBRATION_H

#include <stdint.h>

#include "dsp_pipeline.h"
#include "edge_app.h"
#include "edge_protocol_definitions.h"

// Estado do laço principal, definido em main.c.
OperationModeFSM main_get_active_fsm_mode(void);
AcquisitionMode  main_get_acquisition_mode(void);
float            main_get_requested_sample_rate_hz(void);

void edge_net_send_telemetry(const DSP_AnalysisResult *dsp_res, OperationModeFSM mode,
                             uint16_t dtc_code, const float *fft_mag, uint16_t fft_bins);

#endif /* EDGE_APP_VIBRATION_H */
