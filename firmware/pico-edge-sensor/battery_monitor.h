#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "edge_protocol_definitions.h"

typedef struct {
    bool present;
    uint8_t pct;      // 0..100 ou BATTERY_PCT_UNKNOWN
    uint16_t mv;      // 0xFFFF se desconhecido
    bool low_voltage;
    bool critical_voltage;
} BatteryStatus;

void battery_monitor_init(void);
void battery_monitor_poll(void);
BatteryStatus battery_monitor_get_status(void);
uint8_t battery_monitor_get_pct_for_beacon(void);
uint16_t battery_monitor_get_mv(void);

#endif
