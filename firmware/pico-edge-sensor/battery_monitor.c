#include "battery_monitor.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

extern void diag_report_dtc_event(uint16_t code, uint8_t symptom, uint8_t severity);

#define MAX17048_ADDR          0x36
#define MAX17048_REG_VCELL     0x02
#define MAX17048_REG_SOC       0x04
#define BATTERY_LOW_MV         3500
#define BATTERY_CRITICAL_MV    3300
#define BATTERY_POLL_MS        1000

static BatteryStatus g_batt = {false, BATTERY_PCT_UNKNOWN, 0xFFFF, false, false};
static uint32_t g_last_poll_ms = 0;
static uint32_t g_last_low_dtc_ms = 0;

static bool max17048_read16(uint8_t reg, uint16_t *out) {
    if (!out) return false;
    absolute_time_t t = make_timeout_time_us(5000);
    int wr = i2c_write_blocking_until(i2c1, MAX17048_ADDR, &reg, 1, true, t);
    if (wr < 0) return false;
    uint8_t b[2] = {0};
    t = make_timeout_time_us(5000);
    int rd = i2c_read_blocking_until(i2c1, MAX17048_ADDR, b, 2, false, t);
    if (rd != 2) return false;
    *out = ((uint16_t)b[0] << 8) | b[1];
    return true;
}

void battery_monitor_init(void) {
    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(PIN_I2C1_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_I2C1_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_I2C1_SDA);
    gpio_pull_up(PIN_I2C1_SCL);

    gpio_init(PIN_MAX17_ALRT);
    gpio_set_dir(PIN_MAX17_ALRT, GPIO_IN);
    gpio_pull_up(PIN_MAX17_ALRT);

    g_batt.present = false;
    g_batt.pct = BATTERY_PCT_UNKNOWN;
    g_batt.mv = 0xFFFF;
    g_batt.low_voltage = false;
    g_batt.critical_voltage = false;
    battery_monitor_poll();
}

void battery_monitor_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (g_last_poll_ms != 0 && now - g_last_poll_ms < BATTERY_POLL_MS) return;
    g_last_poll_ms = now;

    uint16_t raw_vcell = 0, raw_soc = 0;
    if (!max17048_read16(MAX17048_REG_VCELL, &raw_vcell) ||
        !max17048_read16(MAX17048_REG_SOC, &raw_soc)) {
        g_batt.present = false;
        g_batt.pct = BATTERY_PCT_UNKNOWN;
        g_batt.mv = 0xFFFF;
        g_batt.low_voltage = false;
        g_batt.critical_voltage = false;
        return;
    }

    // MAX17048: VCELL LSB = 78.125 uV; SOC byte alto = porcentagem inteira.
    uint32_t mv = ((uint32_t)raw_vcell * 78u) / 1000u;
    uint8_t pct = (uint8_t)(raw_soc >> 8);
    if (pct > 100) pct = 100;

    g_batt.present = true;
    g_batt.pct = pct;
    g_batt.mv = (uint16_t)mv;
    g_batt.low_voltage = (mv > 0 && mv < BATTERY_LOW_MV);
    g_batt.critical_voltage = (mv > 0 && mv < BATTERY_CRITICAL_MV);

    if (g_batt.critical_voltage && now - g_last_low_dtc_ms > 5000) {
        g_last_low_dtc_ms = now;
        diag_report_dtc_event(DTC_SYS_LOW_VOLTAGE, FTB_OUT_OF_RANGE_LOW, SEV_CRITICAL);
    } else if (g_batt.low_voltage && now - g_last_low_dtc_ms > 5000) {
        g_last_low_dtc_ms = now;
        diag_report_dtc_event(DTC_SYS_LOW_VOLTAGE, FTB_OUT_OF_RANGE_LOW, SEV_WARNING);
    }
}

BatteryStatus battery_monitor_get_status(void) { return g_batt; }
uint8_t battery_monitor_get_pct_for_beacon(void) { return g_batt.present ? g_batt.pct : BATTERY_PCT_UNKNOWN; }
uint16_t battery_monitor_get_mv(void) { return g_batt.present ? g_batt.mv : 0xFFFF; }
