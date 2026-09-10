/**
 * @file mpu6050_dma_driver.c
 * @brief Driver MPU6050 usando DRDY/INT + leitura I2C fora da ISR.
 *
 * Observação histórica: o nome do arquivo foi mantido para preservar o
 * CMake e os includes existentes, mas esta implementação NÃO usa DMA.
 *
 * Arquitetura:
 * - O pino INT/DRDY do MPU6050 gera borda de subida no GP2.
 * - A ISR de GPIO apenas incrementa um contador de amostras pendentes.
 * - O loop principal chama mpu6050_get_ping_pong_buffer().
 * - Fora da ISR, o driver lê ACCEL_XOUT_H..ACCEL_ZOUT_L via I2C burst.
 * - O ping-pong buffer continua existindo: quando 512 amostras chegam, o
 *   buffer pronto é entregue ao DSP.
 */

#include "mpu6050_dma_driver.h"

#include <string.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

extern void diag_report_dtc_event(uint16_t code, uint8_t symptom, uint8_t severity);

// Buffers ping-pong em RAM.
static MPU_RawSample g_ping_buf[512];
static MPU_RawSample g_pong_buf[512];

static MPU_RawSample *g_write_buf = g_ping_buf;
static MPU_RawSample *g_ready_buf = NULL;

static volatile uint16_t g_sample_idx = 0;
static volatile uint16_t g_active_buffer_size = 512;
static volatile bool     g_buffer_ready = false;
static volatile bool     g_acquisition_active = false;

// Contadores legados de DRDY. Na baseline polling-only devem permanecer em zero.
static volatile uint32_t g_drdy_pending = 0;
static volatile uint32_t g_drdy_irq_count = 0;
static volatile uint32_t g_drdy_missed_count = 0;

static float g_configured_sample_rate_hz = 1000.0f;

#define I2C_TIMEOUT_US_NORMAL             5000
#define I2C_POLLING_ERROR_COOLDOWN_MS     10000
#define I2C_CONSECUTIVE_FAILURES_FOR_DTC  12
#define DRDY_PENDING_SOFT_LIMIT           32

static uint32_t g_last_i2c_error_report_ms = 0;
static uint32_t g_last_overrun_report_ms = 0;
static uint16_t g_i2c_consecutive_failures = 0;

static uint32_t compute_smplrt_divider(uint32_t sample_rate_hz) {
    if (sample_rate_hz < 4)    sample_rate_hz = 4;
    if (sample_rate_hz > 1000) sample_rate_hz = 1000;

    uint32_t divider = (uint32_t)lroundf(1000.0f / (float)sample_rate_hz) - 1;
    if (divider > 255) divider = 255;

    g_configured_sample_rate_hz = 1000.0f / (float)(divider + 1);
    return divider;
}

static void mpu_note_i2c_success(void) {
    g_i2c_consecutive_failures = 0;
}

static void mpu_report_i2c_stuck_throttled(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (g_i2c_consecutive_failures < 0xFFFFu) {
        g_i2c_consecutive_failures++;
    }

    // Em polling a 1000 Hz, uma falha isolada de I2C pode ocorrer por
    // latência temporária, ruído de bancada ou contenção do barramento. Isso não
    // deve virar DTC crítico imediatamente. O DTC 0x1002 representa falha
    // persistente/consecutiva.
    if (g_i2c_consecutive_failures < I2C_CONSECUTIVE_FAILURES_FOR_DTC) {
        return;
    }

    if (now_ms - g_last_i2c_error_report_ms >= I2C_POLLING_ERROR_COOLDOWN_MS) {
        g_last_i2c_error_report_ms = now_ms;
        diag_report_dtc_event(DTC_I2C_BUS_STUCK, FTB_SIGNAL_STUCK_LOW, SEV_CRITICAL);
    }
}

static void mpu_report_overrun_throttled(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (now_ms - g_last_overrun_report_ms >= I2C_POLLING_ERROR_COOLDOWN_MS) {
        g_last_overrun_report_ms = now_ms;
        diag_report_dtc_event(DTC_DSP_OVERRUN, FTB_SIGNAL_RATE_INVALID, SEV_CRITICAL);
    }
}

static void mpu_reset_buffers(void) {
    uint32_t irq_state = save_and_disable_interrupts();

    g_write_buf = g_ping_buf;
    g_ready_buf = NULL;
    g_sample_idx = 0;
    g_buffer_ready = false;
    g_drdy_pending = 0;
    g_drdy_irq_count = 0;
    g_drdy_missed_count = 0;

    restore_interrupts(irq_state);
}

static void mpu_reset_i2c0_peripheral(void) {
    watchdog_update();

    i2c_deinit(i2c0);
    sleep_ms(2);

    gpio_set_function(PIN_I2C0_SDA, GPIO_FUNC_SIO);
    gpio_set_function(PIN_I2C0_SCL, GPIO_FUNC_SIO);
    gpio_pull_up(PIN_I2C0_SDA);
    gpio_pull_up(PIN_I2C0_SCL);

    // Tentativa conservadora de liberar escravo que tenha segurado SDA.
    gpio_set_dir(PIN_I2C0_SDA, GPIO_IN);
    gpio_set_dir(PIN_I2C0_SCL, GPIO_OUT);
    gpio_put(PIN_I2C0_SCL, 1);
    sleep_us(10);

    for (int i = 0; i < 9; i++) {
        gpio_put(PIN_I2C0_SCL, 0);
        sleep_us(10);
        gpio_put(PIN_I2C0_SCL, 1);
        sleep_us(10);
    }

    gpio_set_dir(PIN_I2C0_SDA, GPIO_OUT);
    gpio_put(PIN_I2C0_SDA, 0);
    sleep_us(10);
    gpio_put(PIN_I2C0_SCL, 1);
    sleep_us(10);
    gpio_put(PIN_I2C0_SDA, 1);
    sleep_us(10);

    watchdog_update();

    // 400 kHz é mais seguro para módulos MPU6050 genéricos em protoboard.
    i2c_init(i2c0, 400 * 1000);
    gpio_set_function(PIN_I2C0_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C0_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C0_SDA);
    gpio_pull_up(PIN_I2C0_SCL);
}

static bool mpu_write_reg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    absolute_time_t timeout = make_timeout_time_us(I2C_TIMEOUT_US_NORMAL);
    int ret = i2c_write_blocking_until(i2c0, MPU_ADDR, buf, 2, false, timeout);

    if (ret < 0) {
        mpu_report_i2c_stuck_throttled();
        return false;
    }

    mpu_note_i2c_success();
    return true;
}

static bool mpu_read_regs_ex(uint8_t start_reg, uint8_t *dst, size_t len, bool report_dtc) {
    if (!dst || len == 0) return false;

    absolute_time_t timeout = make_timeout_time_us(I2C_TIMEOUT_US_NORMAL);
    int ret = i2c_write_blocking_until(i2c0, MPU_ADDR, &start_reg, 1, true, timeout);

    if (ret < 0) {
        if (report_dtc) {
            mpu_report_i2c_stuck_throttled();
        }
        return false;
    }

    timeout = make_timeout_time_us(I2C_TIMEOUT_US_NORMAL);
    ret = i2c_read_blocking_until(i2c0, MPU_ADDR, dst, len, false, timeout);

    if (ret < 0 || ret != (int)len) {
        if (report_dtc) {
            mpu_report_i2c_stuck_throttled();
        }
        return false;
    }

    mpu_note_i2c_success();
    return true;
}

static bool mpu_read_regs(uint8_t start_reg, uint8_t *dst, size_t len) {
    return mpu_read_regs_ex(start_reg, dst, len, true);
}

static bool mpu_read_regs_silent(uint8_t start_reg, uint8_t *dst, size_t len) {
    return mpu_read_regs_ex(start_reg, dst, len, false);
}

static bool mpu6050_read_single_sample(MPU_RawSample *sample) {
    if (!sample) return false;
    return mpu_read_regs(MPU_REG_ACCEL_XOUT_H, (uint8_t *)sample, BYTES_PER_SAMPLE);
}

static bool mpu_configure_chip(uint32_t sample_rate_hz, bool enable_drdy_int) {
    uint32_t divider = compute_smplrt_divider(sample_rate_hz);

    bool ok = true;

    ok &= mpu_write_reg(0x6B, 0x00);                 // PWR_MGMT_1: wake up
    sleep_ms(10);
    ok &= mpu_write_reg(0x1A, 0x01);                 // CONFIG: DLPF_CFG=1, base 1 kHz
    ok &= mpu_write_reg(0x19, (uint8_t)divider);     // SMPLRT_DIV
    ok &= mpu_write_reg(0x1C, 0x00);                 // ACCEL_CONFIG: +/-2g

    // INT_PIN_CFG:
    // 0x30 = LATCH_INT_EN + INT_RD_CLEAR.
    // O DRDY fica latched até uma leitura, o que é mais fácil de capturar no GP2
    // do que um pulso estreito.
    ok &= mpu_write_reg(0x37, enable_drdy_int ? 0x30 : 0x00);

    // INT_ENABLE:
    // bit 0 = DATA_RDY_EN.
    ok &= mpu_write_reg(0x38, enable_drdy_int ? 0x01 : 0x00);

    return ok;
}

static void gpio_mpu_drdy_callback(uint gpio, uint32_t events) {
    if (gpio != PIN_MPU_INT_DRDY) {
        return;
    }

    if ((events & GPIO_IRQ_EDGE_RISE) == 0) {
        return;
    }

    g_drdy_irq_count++;

    // ISR mínima: não faz I2C, não imprime, não chama lwIP.
    if (g_drdy_pending < DRDY_PENDING_SOFT_LIMIT) {
        g_drdy_pending++;
    } else {
        g_drdy_missed_count++;
    }
}

static bool mpu_service_one_drdy_sample(void) {
    uint32_t irq_state = save_and_disable_interrupts();

    bool has_pending = (g_drdy_pending > 0);
    if (has_pending) {
        g_drdy_pending--;
    }

    restore_interrupts(irq_state);

    if (!has_pending) {
        return false;
    }

    if (g_buffer_ready) {
        // O DSP ainda não consumiu o buffer anterior. Descarta esta amostra para
        // proteger o buffer pronto e sinaliza overrun de forma limitada.
        mpu_report_overrun_throttled();
        return false;
    }

    MPU_RawSample raw;
    if (!mpu6050_read_single_sample(&raw)) {
        return false;
    }

    g_write_buf[g_sample_idx++] = raw;

    if (g_sample_idx >= g_active_buffer_size) {
        if (g_write_buf == g_ping_buf) {
            g_ready_buf = g_ping_buf;
            g_write_buf = g_pong_buf;
        } else {
            g_ready_buf = g_pong_buf;
            g_write_buf = g_ping_buf;
        }

        g_sample_idx = 0;
        g_buffer_ready = true;
        return true;
    }

    return false;
}

static void mpu_service_drdy_until_buffer_or_empty(void) {
    // Limita o serviço por chamada para não monopolizar o loop principal.
    // Em condições normais, há poucas amostras pendentes por chamada.
    for (int i = 0; i < DRDY_PENDING_SOFT_LIMIT; i++) {
        watchdog_update();

        uint32_t irq_state = save_and_disable_interrupts();
        bool has_pending = (g_drdy_pending > 0);
        restore_interrupts(irq_state);

        if (!has_pending || g_buffer_ready) {
            break;
        }

        (void)mpu_service_one_drdy_sample();
    }
}

void mpu6050_init_dma_driver(void) {
    // Nome mantido por compatibilidade. Baseline atual: polling I2C sem DMA e sem DRDY.
    i2c_init(i2c0, 400 * 1000);
    gpio_set_function(PIN_I2C0_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C0_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C0_SDA);
    gpio_pull_up(PIN_I2C0_SCL);

    gpio_init(PIN_MPU_INT_DRDY);
    gpio_set_dir(PIN_MPU_INT_DRDY, GPIO_IN);
    gpio_pull_down(PIN_MPU_INT_DRDY);

    mpu_reset_buffers();
}

bool mpu6050_check_who_am_i(void) {
    uint8_t val = 0x00;

    if (!mpu_read_regs(0x75, &val, 1)) {
        return false;
    }

    return val == 0x68;
}

void mpu6050_set_buffer_size(uint16_t n) {
    if (!(n == 128 || n == 256 || n == 512)) return;
    uint32_t irq_state = save_and_disable_interrupts();
    g_active_buffer_size = n;
    g_sample_idx = 0;
    g_buffer_ready = false;
    g_ready_buf = NULL;
    g_drdy_pending = 0;
    restore_interrupts(irq_state);
}

uint16_t mpu6050_get_buffer_size(void) {
    return g_active_buffer_size;
}

bool mpu6050_read_temperature_c(float *out_c) {
    if (!out_c) return false;

    // Leitura ambiental usada apenas para freeze frame/STATUS.
    // Falha aqui não deve gerar DTC_I2C_BUS_STUCK, porque pode coincidir com
    // aquisição normal do acelerômetro. O DTC de barramento deve vir do caminho
    // crítico de aquisição/configuração, não de telemetria auxiliar.
    uint8_t raw[2] = {0};
    if (!mpu_read_regs_silent(0x41, raw, sizeof(raw))) return false;

    int16_t temp_raw = (int16_t)((raw[0] << 8) | raw[1]);
    *out_c = ((float)temp_raw / 340.0f) + 36.53f;
    return true;
}

void mpu6050_start_acquisition(uint32_t sample_rate_hz) {
    mpu_reset_buffers();

    if (!mpu_configure_chip(sample_rate_hz, true)) {
        g_acquisition_active = false;
        return;
    }

    // Limpa qualquer INT latched antigo antes de habilitar a borda no GP2.
    // Com INT_PIN_CFG=0x30, qualquer leitura limpa o latch.
    uint8_t dummy[6];
    (void)mpu_read_regs(MPU_REG_ACCEL_XOUT_H, dummy, sizeof(dummy));

    gpio_set_irq_enabled_with_callback(PIN_MPU_INT_DRDY,
                                       GPIO_IRQ_EDGE_RISE,
                                       true,
                                       &gpio_mpu_drdy_callback);

    g_acquisition_active = true;
}

void mpu6050_start_polling_acquisition(uint32_t sample_rate_hz) {
    // Baseline polling-only:
    // - Não habilita INT/DRDY.
    // - Mantém o MPU6050 configurado para amostragem contínua.
    // - A coleta é feita por leituras I2C bloqueantes espaçadas no tempo.
    gpio_set_irq_enabled(PIN_MPU_INT_DRDY, GPIO_IRQ_EDGE_RISE, false);
    mpu_reset_i2c0_peripheral();
    mpu_reset_buffers();

    if (!mpu_configure_chip(sample_rate_hz, false)) {
        g_acquisition_active = false;
        return;
    }

    g_acquisition_active = false;
}


void mpu6050_stop_acquisition(void) {
    gpio_set_irq_enabled(PIN_MPU_INT_DRDY, GPIO_IRQ_EDGE_RISE, false);
    (void)mpu_write_reg(0x38, 0x00); // desabilita DATA_RDY interrupt
    g_acquisition_active = false;
}

float mpu6050_get_configured_sample_rate_hz(void) {
    return g_configured_sample_rate_hz;
}

uint32_t mpu6050_get_drdy_irq_count(void) {
    return g_drdy_irq_count;
}

uint32_t mpu6050_get_drdy_missed_count(void) {
    return g_drdy_missed_count;
}

void mpu6050_reconfigure_sample_rate(uint32_t sample_rate_hz) {
    if (g_acquisition_active) {
        mpu6050_stop_acquisition();
        mpu6050_start_acquisition(sample_rate_hz);
    } else {
        compute_smplrt_divider(sample_rate_hz);
    }
}

bool mpu6050_get_ping_pong_buffer(float *out_x, float *out_y, float *out_z, bool *out_clipping) {
    if (!out_x || !out_y || !out_z || !out_clipping) {
        return false;
    }

    if (g_acquisition_active && !g_buffer_ready) {
        mpu_service_drdy_until_buffer_or_empty();
    }

    if (!g_buffer_ready || g_ready_buf == NULL) {
        return false;
    }

    const float scale_2g = 9.80665f / 16384.0f;
    *out_clipping = false;

    uint32_t irq_state = save_and_disable_interrupts();
    MPU_RawSample *src = g_ready_buf;
    g_ready_buf = NULL;
    g_buffer_ready = false;
    restore_interrupts(irq_state);

    if (src == NULL) {
        return false;
    }

    for (int i = 0; i < g_active_buffer_size; i++) {
        int16_t raw_x = (int16_t)((src[i].x_h << 8) | src[i].x_l);
        int16_t raw_y = (int16_t)((src[i].y_h << 8) | src[i].y_l);
        int16_t raw_z = (int16_t)((src[i].z_h << 8) | src[i].z_l);

        if (raw_x == 32767 || raw_x == -32768 ||
            raw_y == 32767 || raw_y == -32768 ||
            raw_z == 32767 || raw_z == -32768) {
            *out_clipping = true;
        }

        out_x[i] = (float)raw_x * scale_2g;
        out_y[i] = (float)raw_y * scale_2g;
        out_z[i] = (float)raw_z * scale_2g;
    }

    return true;
}

void mpu6050_enter_polling_mode(void) {
    // Mantido para compatibilidade com o fallback existente no main.c.
    // Aqui ele apenas desliga o DRDY e configura o chip sem interrupção.
    gpio_set_irq_enabled(PIN_MPU_INT_DRDY, GPIO_IRQ_EDGE_RISE, false);
    mpu_reset_i2c0_peripheral();
    mpu_reset_buffers();
    (void)mpu_configure_chip((uint32_t)g_configured_sample_rate_hz, false);
    g_acquisition_active = false;
}

bool mpu6050_get_polling_buffer(float *out_x, float *out_y, float *out_z, bool *out_clipping) {
    if (!out_x || !out_y || !out_z || !out_clipping) {
        return false;
    }

    const float scale_2g = 9.80665f / 16384.0f;
    float fs_hz = g_configured_sample_rate_hz;

    if (fs_hz < 1.0f) {
        fs_hz = 1000.0f;
    }

    uint32_t sample_period_us = (uint32_t)(1000000.0f / fs_hz);
    if (sample_period_us == 0) {
        sample_period_us = 1;
    }

    *out_clipping = false;

    for (int i = 0; i < g_active_buffer_size; i++) {
        watchdog_update();

        absolute_time_t t0 = get_absolute_time();

        MPU_RawSample raw;
        if (!mpu6050_read_single_sample(&raw)) {
            return false;
        }

        int16_t raw_x = (int16_t)((raw.x_h << 8) | raw.x_l);
        int16_t raw_y = (int16_t)((raw.y_h << 8) | raw.y_l);
        int16_t raw_z = (int16_t)((raw.z_h << 8) | raw.z_l);

        if (raw_x == 32767 || raw_x == -32768 ||
            raw_y == 32767 || raw_y == -32768 ||
            raw_z == 32767 || raw_z == -32768) {
            *out_clipping = true;
        }

        out_x[i] = (float)raw_x * scale_2g;
        out_y[i] = (float)raw_y * scale_2g;
        out_z[i] = (float)raw_z * scale_2g;

        uint32_t elapsed_us = (uint32_t)absolute_time_diff_us(t0, get_absolute_time());
        if (elapsed_us < sample_period_us) {
            sleep_us(sample_period_us - elapsed_us);
        }
    }

    return true;
}
