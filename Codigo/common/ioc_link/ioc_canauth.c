#include "ioc_canauth.h"

#include <string.h>

#include "ioc_secpdu.h"

static uint16_t kind_id(int kind) {
    return kind == 0 ? (uint16_t)IOC_CA_ID_DATA_CMD : (uint16_t)IOC_CA_ID_TEST_CMD;
}

int ioc_ca_kind_of(uint16_t can_id) {
    if (can_id == IOC_CA_ID_DATA_CMD) return 0;
    if (can_id == IOC_CA_ID_TEST_CMD) return 1;
    return -1;
}

void ioc_ca_tx_init(ioc_ca_tx *tx, const uint8_t bus_key[IOC_LINK_KEY_LEN], uint64_t saved) {
    memcpy(tx->key, bus_key, IOC_LINK_KEY_LEN);
    tx->freshness = saved;
}

uint64_t ioc_ca_tx_boot_ceiling(const ioc_ca_tx *tx) {
    return tx->freshness + IOC_CA_TX_SAVE_STEP;
}

int ioc_ca_tx_needs_save(const ioc_ca_tx *tx, uint64_t saved) {
    return tx->freshness + 1u > saved;
}

uint64_t ioc_ca_tx_next_ceiling(const ioc_ca_tx *tx) {
    return tx->freshness + IOC_CA_TX_SAVE_STEP;
}

int ioc_ca_tx_make_tag(ioc_ca_tx *tx, uint16_t can_id, const uint8_t *data, uint8_t len,
                       uint8_t tag[IOC_CA_TAG_LEN]) {
    const int kind = ioc_ca_kind_of(can_id);
    if (kind < 0 || len != 8 || !data) return 0;
    uint8_t pdu[8 + IOC_CA_FRESH_LEN + IOC_CA_MAC_LEN];
    if (ioc_secpdu_protect_ex(tx->key, can_id, data, len, &tx->freshness, IOC_CA_FRESH_LEN, IOC_CA_MAC_LEN, pdu,
                              sizeof(pdu)) == 0) {
        return 0;
    }
    tag[0] = (uint8_t)kind;
    memcpy(&tag[1], &pdu[len], IOC_CA_FRESH_LEN + IOC_CA_MAC_LEN);
    return 1;
}

void ioc_ca_rx_init(ioc_ca_rx *rx, const uint8_t bus_key[IOC_LINK_KEY_LEN], uint64_t saved) {
    memset(rx, 0, sizeof(*rx));
    memcpy(rx->key, bus_key, IOC_LINK_KEY_LEN);
    rx->freshness = saved;
}

ioc_ca_action ioc_ca_rx_frame(ioc_ca_rx *rx, uint32_t now_ms, uint16_t can_id, const uint8_t *data, uint8_t len,
                              ioc_ca_frame *released, ioc_ca_why *why) {
    ioc_ca_why dummy;
    if (!why) why = &dummy;
    *why = IOC_CA_WHY_NONE;

    const int kind = ioc_ca_kind_of(can_id);
    if (kind >= 0) {
        if (len != 8) {
            *why = IOC_CA_WHY_BAD_LENGTH;
            return IOC_CA_DROP;
        }
        ioc_ca_pending *slot = &rx->pending[kind];
        if (slot->valid) *why = IOC_CA_WHY_REPLACED;   /* o anterior nunca recebeu etiqueta */
        slot->valid = 1;
        slot->len = len;
        memcpy(slot->data, data, len);
        slot->at_ms = now_ms;
        return IOC_CA_HOLD;
    }

    if (can_id != IOC_CA_TAG_ID) return IOC_CA_PASS;

    if (len != IOC_CA_TAG_LEN) {
        *why = IOC_CA_WHY_BAD_LENGTH;
        return IOC_CA_DROP;
    }
    if (data[0] >= IOC_CA_KINDS) {
        *why = IOC_CA_WHY_BAD_KIND;
        return IOC_CA_DROP;
    }
    ioc_ca_pending *slot = &rx->pending[data[0]];
    if (slot->valid && (uint32_t)(now_ms - slot->at_ms) > IOC_CA_TAG_WAIT_MS) {
        slot->valid = 0;   /* venceu: a etiqueta chegou tarde demais para este comando */
        ++rx->no_tag;
    }
    if (!slot->valid) {
        ++rx->orphan_tag;
        *why = IOC_CA_WHY_ORPHAN_TAG;
        return IOC_CA_DROP;
    }

    uint8_t pdu[8 + IOC_CA_FRESH_LEN + IOC_CA_MAC_LEN];
    memcpy(pdu, slot->data, slot->len);
    memcpy(&pdu[slot->len], &data[1], IOC_CA_FRESH_LEN + IOC_CA_MAC_LEN);
    uint64_t fresh = rx->freshness;
    size_t payload_len = 0;
    if (!ioc_secpdu_verify_ex(rx->key, kind_id(data[0]), pdu, (size_t)slot->len + IOC_CA_FRESH_LEN + IOC_CA_MAC_LEN,
                              &fresh, IOC_CA_FRESH_LEN, IOC_CA_MAC_LEN, &payload_len)) {
        /* O comando continua guardado: uma etiqueta falsa não deve inutilizar o comando legítimo que ainda
         * pode chegar; o prazo de espera o descarta. */
        ++rx->bad_tag;
        *why = IOC_CA_WHY_BAD_TAG;
        return IOC_CA_DROP;
    }
    rx->freshness = fresh;
    ++rx->accepted;
    if (released) {
        released->id = kind_id(data[0]);
        released->len = slot->len;
        memcpy(released->data, slot->data, slot->len);
    }
    slot->valid = 0;
    return IOC_CA_RELEASE;
}

uint16_t ioc_ca_rx_expire(ioc_ca_rx *rx, uint32_t now_ms) {
    for (uint8_t kind = 0; kind < IOC_CA_KINDS; ++kind) {
        ioc_ca_pending *slot = &rx->pending[kind];
        if (slot->valid && (uint32_t)(now_ms - slot->at_ms) > IOC_CA_TAG_WAIT_MS) {
            slot->valid = 0;
            ++rx->no_tag;
            return kind_id(kind);
        }
    }
    return 0;
}

int ioc_ca_rx_needs_save(const ioc_ca_rx *rx, uint64_t saved) {
    return rx->freshness >= saved + IOC_CA_RX_SAVE_STEP;
}

const char *ioc_ca_why_name(ioc_ca_why why) {
    switch (why) {
        case IOC_CA_WHY_BAD_LENGTH: return "TAMANHO";
        case IOC_CA_WHY_BAD_KIND: return "TIPO";
        case IOC_CA_WHY_ORPHAN_TAG: return "ETIQUETA_SEM_COMANDO";
        case IOC_CA_WHY_BAD_TAG: return "ETIQUETA_INVALIDA";
        case IOC_CA_WHY_NO_TAG: return "SEM_ETIQUETA";
        case IOC_CA_WHY_REPLACED: return "COMANDO_SUBSTITUIDO";
        default: return "NONE";
    }
}
