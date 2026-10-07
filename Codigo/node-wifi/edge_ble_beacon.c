\
#include "edge_ble_beacon.h"

#include <stdint.h>
#include <string.h>

#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "device_identity.h"
#include "edge_protocol_definitions.h"

#define EDGE_BLE_COMPANY_ID_LO 0xFF
#define EDGE_BLE_COMPANY_ID_HI 0xFF
#define EDGE_BLE_MAGIC_0       0x49 /* 'I' */
#define EDGE_BLE_MAGIC_1       0x43 /* 'C' */
#define EDGE_BLE_ADV_VERSION   0x01

/* Varredura passiva de ofertas: 60 ms a cada 500 ms (12% do tempo de rádio).
   O Node anuncia a oferta a cada ~100 ms, então alguns segundos bastam. */
#define EDGE_BLE_SCAN_INTERVAL 0x0320 /* 500 ms em unidades de 0,625 ms */
#define EDGE_BLE_SCAN_WINDOW   0x0060 /* 60 ms */

static bool g_ble_active = false;
static bool g_stack_ready = false;
static bool g_scan_wanted = false;
static bool g_scan_active = false;
static edge_ble_mfg_handler_t g_mfg_handler = NULL;
static btstack_packet_callback_registration_t g_hci_event_callback;
static uint8_t g_adv_data[31];
static uint8_t g_adv_len = 0;

static void build_advertisement(void) {
    uint64_t uuid = edge_device_uuid64();
    uint8_t i = 0;

    /* Flags: general discoverable, BR/EDR not supported. */
    g_adv_data[i++] = 0x02;
    g_adv_data[i++] = BLUETOOTH_DATA_TYPE_FLAGS;
    g_adv_data[i++] = 0x06;

    /* Manufacturer data: company(2), magic(2), adv-version, profile,
       protocol-version, UUID64 big endian. */
    g_adv_data[i++] = 0x10;
    g_adv_data[i++] = BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA;
    g_adv_data[i++] = EDGE_BLE_COMPANY_ID_LO;
    g_adv_data[i++] = EDGE_BLE_COMPANY_ID_HI;
    g_adv_data[i++] = EDGE_BLE_MAGIC_0;
    g_adv_data[i++] = EDGE_BLE_MAGIC_1;
    g_adv_data[i++] = EDGE_BLE_ADV_VERSION;
    g_adv_data[i++] = (uint8_t)PROFILE_ACCELEROMETER;
    g_adv_data[i++] = NET_PROTOCOL_VERSION;
    for (int shift = 56; shift >= 0; shift -= 8) {
        g_adv_data[i++] = (uint8_t)((uuid >> shift) & 0xFFu);
    }

    /* Short local name keeps the advertisement under the 31-byte limit. */
    static const char name[] = "IOTCAN";
    g_adv_data[i++] = (uint8_t)(1 + sizeof(name) - 1);
    g_adv_data[i++] = BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME;
    memcpy(&g_adv_data[i], name, sizeof(name) - 1);
    i += (uint8_t)(sizeof(name) - 1);

    g_adv_len = i;
}

static void start_advertising(void) {
    uint16_t adv_int_min = 0x0320; /* 500 ms in 0.625 ms units */
    uint16_t adv_int_max = 0x0320;
    uint8_t adv_type = 0;          /* connectable undirected; association follows later */
    bd_addr_t null_addr;
    memset(null_addr, 0, sizeof(null_addr));

    build_advertisement();
    gap_advertisements_set_params(adv_int_min, adv_int_max, adv_type, 0,
                                  null_addr, 0x07, 0x00);
    gap_advertisements_set_data(g_adv_len, g_adv_data);
    gap_advertisements_enable(1);
    g_ble_active = true;
}

/* Deve ser chamada com o contexto assíncrono do CYW43 já adquirido. */
static void apply_scan_state(void) {
    if (!g_stack_ready) return;
    if (g_scan_wanted && !g_scan_active) {
        gap_set_scan_params(0 /* passiva */, EDGE_BLE_SCAN_INTERVAL, EDGE_BLE_SCAN_WINDOW, 0);
        gap_start_scan();
        g_scan_active = true;
    } else if (!g_scan_wanted && g_scan_active) {
        gap_stop_scan();
        g_scan_active = false;
    }
}

static void handle_advertising_report(uint8_t *packet) {
    if (!g_mfg_handler) return;
    const uint8_t *data = gap_event_advertising_report_get_data(packet);
    uint8_t length = gap_event_advertising_report_get_data_length(packet);
    ad_context_t context;
    for (ad_iterator_init(&context, length, data); ad_iterator_has_more(&context);
         ad_iterator_next(&context)) {
        if (ad_iterator_get_data_type(&context) != BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA) {
            continue;
        }
        g_mfg_handler(ad_iterator_get_data(&context), ad_iterator_get_data_len(&context));
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                g_stack_ready = true;
                start_advertising();
                apply_scan_state();
            }
            break;
        case GAP_EVENT_ADVERTISING_REPORT:
            handle_advertising_report(packet);
            break;
        default:
            break;
    }
}

bool edge_ble_beacon_init(void) {
    g_ble_active = false;
    g_stack_ready = false;
    g_scan_active = false;
    l2cap_init();
    g_hci_event_callback.callback = &packet_handler;
    hci_add_event_handler(&g_hci_event_callback);
    return hci_power_control(HCI_POWER_ON) == ERROR_CODE_SUCCESS;
}

bool edge_ble_beacon_is_active(void) {
    return g_ble_active;
}

void edge_ble_offer_scan_set_handler(edge_ble_mfg_handler_t handler) {
    g_mfg_handler = handler;
}

void edge_ble_offer_scan_enable(bool enable) {
    /* As chamadas gap_* precisam do mesmo lock usado pelo lwIP/BTstack no
       modo threadsafe_background. */
    cyw43_thread_enter();
    g_scan_wanted = enable;
    apply_scan_state();
    cyw43_thread_exit();
}

bool edge_ble_offer_scan_is_active(void) {
    return g_scan_active;
}
