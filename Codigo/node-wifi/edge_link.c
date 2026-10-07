#include "edge_link.h"

#include <stdlib.h>
#include <string.h>

#include "ioc_link.h"
#include "ioc_sha256.h"

#ifndef EDGE_LINK_DEVICE_KEY_HEX
#define EDGE_LINK_DEVICE_KEY_HEX ""
#endif
#ifndef EDGE_LINK_NETWORK_KEY_HEX
#define EDGE_LINK_NETWORK_KEY_HEX ""
#endif
#ifndef EDGE_LINK_MASTER_KEY_HEX
#define EDGE_LINK_MASTER_KEY_HEX ""
#endif
#ifndef EDGE_LINK_DEVICE_UUID_HEX
#define EDGE_LINK_DEVICE_UUID_HEX ""
#endif

static uint8_t         g_device_key[IOC_LINK_KEY_LEN];
static uint8_t         g_network_key[IOC_LINK_KEY_LEN];
static EdgeLinkKeyMode g_mode = EDGE_LINK_KEYS_NONE;

void edge_link_init(uint64_t device_uuid) {
    uint8_t master[IOC_LINK_KEY_LEN];

    memset(g_device_key, 0, sizeof(g_device_key));
    memset(g_network_key, 0, sizeof(g_network_key));
    g_mode = EDGE_LINK_KEYS_NONE;

    if (ioc_link_hex_to_key(EDGE_LINK_DEVICE_KEY_HEX, g_device_key) &&
        ioc_link_hex_to_key(EDGE_LINK_NETWORK_KEY_HEX, g_network_key) &&
        !ioc_link_key_is_zero(g_device_key) && !ioc_link_key_is_zero(g_network_key)) {
        /* K_dev só vale para um UUID. Se o build informou para qual sensor a
           chave foi derivada, um firmware gravado na placa errada é recusado. */
        const char *expected = EDGE_LINK_DEVICE_UUID_HEX;
        if (expected[0] != '\0' && strtoull(expected, NULL, 16) != device_uuid) {
            memset(g_device_key, 0, sizeof(g_device_key));
            memset(g_network_key, 0, sizeof(g_network_key));
            g_mode = EDGE_LINK_KEYS_UUID_MISMATCH;
            return;
        }
        g_mode = EDGE_LINK_KEYS_DEVICE;
        return;
    }

    if (ioc_link_hex_to_key(EDGE_LINK_MASTER_KEY_HEX, master) && !ioc_link_key_is_zero(master)) {
        ioc_link_derive_device_key(master, device_uuid, g_device_key);
        ioc_link_derive_network_key(master, g_network_key);
        ioc_secure_zero(master, sizeof(master));
        g_mode = EDGE_LINK_KEYS_MASTER;
        return;
    }

    memset(g_device_key, 0, sizeof(g_device_key));
    memset(g_network_key, 0, sizeof(g_network_key));
}

bool edge_link_is_provisioned(void) {
    return g_mode == EDGE_LINK_KEYS_DEVICE || g_mode == EDGE_LINK_KEYS_MASTER;
}

EdgeLinkKeyMode edge_link_key_mode(void) { return g_mode; }

const char *edge_link_key_mode_name(void) {
    switch (g_mode) {
        case EDGE_LINK_KEYS_DEVICE:        return "DEVICE";
        case EDGE_LINK_KEYS_MASTER:        return "MASTER";
        case EDGE_LINK_KEYS_UUID_MISMATCH: return "UUID_MISMATCH";
        default:                           return "NONE";
    }
}

const uint8_t *edge_link_device_key(void) { return g_device_key; }
const uint8_t *edge_link_network_key(void) { return g_network_key; }
