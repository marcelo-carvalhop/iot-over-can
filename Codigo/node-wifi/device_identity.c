#include "device_identity.h"
#include "pico/unique_id.h"

_Static_assert(PICO_UNIQUE_BOARD_ID_SIZE_BYTES == 8, "iot-over-can protocol expects a 64-bit Pico unique ID");

static uint64_t g_uuid64 = 0;

void edge_device_identity_init(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    uint64_t value = 0;
    for (unsigned i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; ++i) {
        value = (value << 8) | (uint64_t)id.id[i];
    }
    /* Zero is reserved as unknown/unprovisioned in our protocol. */
    g_uuid64 = value ? value : 1u;
}

uint64_t edge_device_uuid64(void) {
    if (g_uuid64 == 0) edge_device_identity_init();
    return g_uuid64;
}
