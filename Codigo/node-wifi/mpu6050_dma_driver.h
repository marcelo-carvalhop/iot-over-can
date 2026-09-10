/**
 * @file mpu6050_dma_driver.h
 * @brief Driver I²C0 com DRDY/INT e Ping-Pong buffering, sem DMA.
 * O nome do arquivo/API foi mantido por compatibilidade com o restante do firmware.
 */
#ifndef MPU6050_DMA_DRIVER_H
#define MPU6050_DMA_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "edge_protocol_definitions.h"

#define MPU_ADDR 0x68
#define MPU_REG_ACCEL_XOUT_H 0x3B
#define BYTES_PER_SAMPLE 6

typedef struct __attribute__((packed)) {
    uint8_t x_h, x_l;
    uint8_t y_h, y_l;
    uint8_t z_h, z_l;
} MPU_RawSample;

void mpu6050_init_dma_driver(void);
bool mpu6050_check_who_am_i(void);
void mpu6050_start_acquisition(uint32_t sample_rate_hz); // legado/experimental: DRDY
void mpu6050_start_polling_acquisition(uint32_t sample_rate_hz);
void mpu6050_stop_acquisition(void);
bool mpu6050_get_ping_pong_buffer(float *out_x, float *out_y, float *out_z, bool *out_clipping);
void mpu6050_set_buffer_size(uint16_t n);
uint16_t mpu6050_get_buffer_size(void);
bool mpu6050_read_temperature_c(float *out_c);

// Fallback de bancada: coleta um buffer por leitura bloqueante/polling via I2C0.
// Útil quando WHO_AM_I funciona, mas o pino INT/DRDY não gera ping-pong buffer.
bool mpu6050_get_polling_buffer(float *out_x, float *out_y, float *out_z, bool *out_clipping);
// Desabilita IRQ/DRDY e deixa o driver em modo polling estável para bancada.
void mpu6050_enter_polling_mode(void);
// Item 9: retorna a ODR REALMENTE configurada no chip (pode diferir
// levemente do sample_rate_hz pedido, pois o divisor é inteiro). Use este
// valor — não uma constante hardcoded — em qualquer cálculo que dependa da
// taxa de amostragem (FFT, PPV, STA/LTA).
float mpu6050_get_configured_sample_rate_hz(void);
uint32_t mpu6050_get_drdy_irq_count(void);
uint32_t mpu6050_get_drdy_missed_count(void);
// Reconfigura a taxa em runtime; funciona com ou sem hardware ativo (ver
// comentário no .c). Use esta função (não mpu6050_start_acquisition
// diretamente) para aplicar uma mudança de sample_rate_hz vinda de
// CMD_SET_CONFIG ou do console serial.
void mpu6050_reconfigure_sample_rate(uint32_t sample_rate_hz);

#endif // MPU6050_DMA_DRIVER_H