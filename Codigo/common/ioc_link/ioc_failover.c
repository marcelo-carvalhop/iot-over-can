#include "ioc_failover.h"

#include <string.h>

uint8_t ioc_assoc_pack(uint8_t state, int auto_failover, int secure) {
    if (state == IOC_ASSOC_REJECTED) return IOC_ASSOC_REJECTED;
    return (uint8_t)((state & 0x3Fu) | (auto_failover ? IOC_ASSOC_FLAG_AUTO : 0) |
                     (secure ? IOC_ASSOC_FLAG_SECURE : 0));
}

void ioc_assoc_unpack(uint8_t raw, uint8_t *state, int *auto_failover, int *secure) {
    if (raw == IOC_ASSOC_REJECTED) {
        if (state) *state = IOC_ASSOC_REJECTED;
        if (auto_failover) *auto_failover = 0;
        if (secure) *secure = 0;
        return;
    }
    if (state) *state = (uint8_t)(raw & 0x3Fu);
    if (auto_failover) *auto_failover = (raw & IOC_ASSOC_FLAG_AUTO) != 0;
    if (secure) *secure = (raw & IOC_ASSOC_FLAG_SECURE) != 0;
}

static int state_is_active(uint8_t state) {
    return state == IOC_ASSOC_ASSOCIATING || state == IOC_ASSOC_BOUND || state == IOC_ASSOC_ONLINE;
}

static int state_is_out_of_reach(uint8_t state) {
    return state == IOC_ASSOC_STALE || state == IOC_ASSOC_LOST;
}

static int state_is_binding(uint8_t state) {
    return state_is_active(state) || state == IOC_ASSOC_STALE || state == IOC_ASSOC_LOST;
}

void ioc_fo_init(ioc_fo_table *table) {
    if (table) memset(table, 0, sizeof(*table));
}

static ioc_fo_entry *find_entry(ioc_fo_table *table, uint64_t uuid) {
    for (unsigned i = 0; i < IOC_FO_MAX_ENTRIES; ++i) {
        if (table->entries[i].used && table->entries[i].uuid == uuid) return &table->entries[i];
    }
    return NULL;
}

const ioc_fo_entry *ioc_fo_find(const ioc_fo_table *table, uint64_t uuid) {
    if (!table) return NULL;
    for (unsigned i = 0; i < IOC_FO_MAX_ENTRIES; ++i) {
        if (table->entries[i].used && table->entries[i].uuid == uuid) return &table->entries[i];
    }
    return NULL;
}

static ioc_fo_entry *allocate_entry(ioc_fo_table *table, uint32_t now_ms) {
    ioc_fo_entry *oldest = &table->entries[0];
    for (unsigned i = 0; i < IOC_FO_MAX_ENTRIES; ++i) {
        ioc_fo_entry *entry = &table->entries[i];
        if (!entry->used) return entry;
        if (now_ms - entry->status_ms > now_ms - oldest->status_ms) oldest = entry;
    }
    return oldest; /* tabela cheia: reaproveita o vínculo sem notícias há mais tempo */
}

void ioc_fo_on_status(ioc_fo_table *table, uint32_t now_ms, uint8_t node, uint8_t child,
                      uint64_t uuid, uint8_t state, int auto_failover, int secure) {
    if (!table || uuid == 0 || node == 0) return;
    ioc_fo_entry *entry = find_entry(table, uuid);

    if (state == IOC_ASSOC_UNBOUND) {
        /* Só o responsável registrado encerra o vínculo na tabela. */
        if (entry && entry->owner == node) memset(entry, 0, sizeof(*entry));
        return;
    }
    if (!state_is_binding(state)) return; /* REJECTED, DISCOVERED ou valor desconhecido */

    if (!entry) {
        entry = allocate_entry(table, now_ms);
        memset(entry, 0, sizeof(*entry));
        entry->used = 1;
        entry->uuid = uuid;
        entry->owner = node;
    } else if (entry->owner != node) {
        /*
         * Outro Node publica o mesmo sensor. A troca de responsável é aceita
         * quando o novo tem sessão com o sensor, ou quando o registrado não
         * tem sessão recente. Um Node que voltou de uma ausência, sem sessão,
         * não desloca o responsável que está de fato recebendo dados.
         */
        const int current_holds = entry->secure && !entry->owner_faulted &&
                                  now_ms - entry->status_ms <= IOC_FO_OWNER_SILENT_MS;
        if (!state_is_active(state) || (current_holds && !secure)) return;
        /* O intervalo mínimo entre decisões continua valendo para o novo
         * responsável: action_taken e action_ms não são alterados. */
        entry->owner = node;
    }

    entry->child = child;
    entry->state = state;
    entry->auto_failover = auto_failover ? 1 : 0;
    entry->secure = secure ? 1 : 0;
    entry->owner_faulted = 0;
    entry->status_ms = now_ms;
}

void ioc_fo_on_observation(ioc_fo_table *table, uint32_t now_ms, uint8_t node, uint64_t uuid,
                           int8_t rssi) {
    if (!table || node == 0) return;
    ioc_fo_entry *entry = find_entry(table, uuid);
    if (!entry) return; /* só interessam sensores com vínculo */

    ioc_fo_observation *slot = NULL;
    ioc_fo_observation *oldest = &entry->obs[0];
    for (unsigned i = 0; i < IOC_FO_MAX_OBSERVERS; ++i) {
        ioc_fo_observation *obs = &entry->obs[i];
        if (obs->node == node) {
            slot = obs;
            break;
        }
        if (obs->node == 0) {
            if (!slot) slot = obs;
        } else if (now_ms - obs->seen_ms > now_ms - oldest->seen_ms) {
            oldest = obs;
        }
    }
    if (!slot) slot = oldest;
    slot->node = node;
    slot->rssi = rssi;
    slot->seen_ms = now_ms;
}

void ioc_fo_on_node_fault(ioc_fo_table *table, uint8_t node) {
    if (!table || node == 0) return;
    for (unsigned i = 0; i < IOC_FO_MAX_ENTRIES; ++i) {
        ioc_fo_entry *entry = &table->entries[i];
        if (!entry->used) continue;
        if (entry->owner == node) entry->owner_faulted = 1;
        /* Um Node ausente não pode receber sensores: o que ele observou
         * antes de sair do ar deixa de valer. */
        for (unsigned k = 0; k < IOC_FO_MAX_OBSERVERS; ++k) {
            if (entry->obs[k].node == node) memset(&entry->obs[k], 0, sizeof(entry->obs[k]));
        }
    }
}

void ioc_fo_on_local_stall(ioc_fo_table *table, uint32_t now_ms) {
    if (!table) return;
    for (unsigned i = 0; i < IOC_FO_MAX_ENTRIES; ++i) {
        ioc_fo_entry *entry = &table->entries[i];
        if (!entry->used) continue;
        entry->status_ms = now_ms;
        memset(entry->obs, 0, sizeof(entry->obs));
    }
}

int ioc_fo_poll(ioc_fo_table *table, uint32_t now_ms, ioc_fo_decision *out) {
    if (!table || !out) return 0;

    for (unsigned i = 0; i < IOC_FO_MAX_ENTRIES; ++i) {
        ioc_fo_entry *entry = &table->entries[i];
        if (!entry->used) continue;

        const uint32_t silent_ms = now_ms - entry->status_ms;
        if (silent_ms > IOC_FO_PURGE_MS) {
            memset(entry, 0, sizeof(*entry));
            continue;
        }
        if (!entry->auto_failover) continue;
        if (entry->action_taken && now_ms - entry->action_ms < IOC_FO_COOLDOWN_MS) continue;

        uint8_t reason = IOC_FO_REASON_NONE;
        if (entry->owner_faulted || silent_ms > IOC_FO_OWNER_SILENT_MS) {
            reason = IOC_FO_REASON_OWNER_LOST;
        } else if (state_is_out_of_reach(entry->state) && !entry->secure) {
            /* O responsável não ouve o sensor (STALE ou LOST) nem tem sessão
             * com ele. Com sessão ativa o sensor está entregando dados, ainda
             * que o rádio BLE do responsável não o esteja ouvindo. */
            reason = IOC_FO_REASON_SENSOR_LOST;
        }
        if (reason == IOC_FO_REASON_NONE) continue;

        const ioc_fo_observation *best = NULL;
        for (unsigned k = 0; k < IOC_FO_MAX_OBSERVERS; ++k) {
            const ioc_fo_observation *obs = &entry->obs[k];
            if (obs->node == 0) continue;
            if (obs->node == entry->owner) {
                if (reason != IOC_FO_REASON_OWNER_LOST) continue;
                /*
                 * O responsável só concorre se voltou a observar o sensor bem
                 * depois do último estado que publicou: sinal de que reiniciou
                 * e perdeu o vínculo. Uma observação de antes da falha não conta.
                 */
                if ((int32_t)(obs->seen_ms - entry->status_ms) < (int32_t)IOC_FO_OWNER_RETURN_MS) continue;
            }
            if (now_ms - obs->seen_ms > IOC_FO_OBSERVATION_AGE_MS) continue;
            if (obs->rssi < IOC_FO_MIN_RSSI) continue;
            if (!best || obs->rssi > best->rssi ||
                (obs->rssi == best->rssi && obs->node < best->node)) {
                best = obs;
            }
        }
        if (!best) continue; /* nenhum outro Node enxerga o sensor: nada a fazer */

        out->uuid = entry->uuid;
        out->from_node = entry->owner;
        out->to_node = best->node;
        out->reason = reason;
        out->rssi = best->rssi;
        entry->action_taken = 1;
        entry->action_ms = now_ms;
        return 1;
    }
    return 0;
}

int ioc_fo_should_yield(uint8_t self_node, uint8_t self_state, int self_secure,
                        uint8_t other_node, uint8_t other_state, int other_secure) {
    if (other_node == 0 || other_node == self_node) return 0;
    if (!state_is_active(other_state)) return 0; /* o outro está perdendo o sensor */
    if (self_secure) return 0;
    if (other_secure) return 1;
    if (state_is_out_of_reach(self_state)) return 1;
    return self_node < other_node;
}
