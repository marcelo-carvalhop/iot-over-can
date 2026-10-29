#include "ioc_panel_cache.h"

#include <string.h>

#include "ioc_panel_canids.h"
#include "ioc_wdata.h"

static int is_data(uint16_t id) { return id > IOC_ID_DATA_BASE && id <= IOC_ID_DATA_LAST; }

ioc_pclass ioc_pcache_classify(uint16_t id, const uint8_t *data, uint8_t len, uint8_t *key) {
    uint8_t k = 0;
    ioc_pclass cls = IOC_PCLASS_LIVE;
    if (id == IOC_ID_LOAD_HIGH || id == IOC_ID_LOAD_LOW || id == IOC_ID_DATA_CMD) {
        cls = IOC_PCLASS_DROP;
    } else if (id == IOC_ID_ELECTION) {
        cls = IOC_PCLASS_EVENT;
    } else if (id == IOC_ID_LEADER) {
        cls = IOC_PCLASS_BOTH;
    } else if (id == IOC_ID_HEARTBEAT) {
        cls = IOC_PCLASS_STATE;
        k = len > 1 ? data[1] : 0;
    } else if (id == IOC_ID_SENSOR) {
        cls = IOC_PCLASS_STATE;
        k = len > 0 ? data[0] : 0;
    } else if (id == IOC_ID_CONTROL) {
        if (len >= 4 && data[0] == IOC_STATUS_OPCODE &&
            (data[1] == IOC_SUB_STATE_UPDATE || data[1] == IOC_SUB_STATE_FORCE || data[1] == IOC_SUB_STATUS_RESP)) {
            cls = IOC_PCLASS_STATE;
            k = data[2];
        } else {
            cls = IOC_PCLASS_EVENT;
        }
    } else if (id >= IOC_ID_DISCOVERY_BASE && id <= IOC_ID_DISCOVERY_LAST) {
        cls = IOC_PCLASS_STATE;
    } else if (id >= IOC_ID_ASSOC_CMD_FIRST && id <= IOC_ID_ASSOC_CMD_LAST) {
        cls = IOC_PCLASS_EVENT;
    } else if (id == IOC_ID_TEST_CMD || id == IOC_ID_TEST_STATUS) {
        cls = IOC_PCLASS_EVENT;
    } else if (id >= IOC_ID_ASSOC_STATUS_BASE && id <= IOC_ID_ASSOC_STATUS_LAST) {
        cls = IOC_PCLASS_STATE;
        k = len > 1 ? data[1] : 0;
    } else if (is_data(id)) {
        cls = (len == 8 && (data[0] & 0x1Fu) != IOC_WD_KIND_SPECTRUM) ? IOC_PCLASS_XFER : IOC_PCLASS_DROP;
    }
    if (key) *key = k;
    return cls;
}

int ioc_pcache_forward_live(uint16_t id, const uint8_t *data, uint8_t len) {
    return ioc_pcache_classify(id, data, len, NULL) != IOC_PCLASS_DROP;
}

void ioc_pcache_init(ioc_pcache *cache) {
    if (cache) memset(cache, 0, sizeof(*cache));
}

static void mailbox_push(ioc_pcache *c, uint16_t id, const uint8_t *data, uint8_t len, uint8_t own, uint32_t now) {
    if (!c->mailbox_open) return;
    if (c->mb_count == IOC_PCACHE_MAILBOX) {
        c->mb_head = (uint8_t)((c->mb_head + 1u) % IOC_PCACHE_MAILBOX);
        c->mb_count--;
        if (c->mb_dropped < 0xFFFFu) c->mb_dropped++;
    }
    ioc_pcache_entry *e = &c->mailbox[(c->mb_head + c->mb_count) % IOC_PCACHE_MAILBOX];
    e->used = 1;
    e->id = id;
    e->own = own;
    e->len = len > 8 ? 8 : len;
    e->ms = now;
    memcpy(e->data, data, e->len);
    c->mb_count++;
}

static ioc_pcache_entry *state_slot(ioc_pcache *c, uint16_t id, uint8_t key) {
    ioc_pcache_entry *free_slot = NULL;
    ioc_pcache_entry *oldest = &c->state[0];
    for (unsigned i = 0; i < IOC_PCACHE_STATE_SLOTS; ++i) {
        ioc_pcache_entry *e = &c->state[i];
        if (e->used && e->id == id && e->key == key) return e;
        if (!e->used && !free_slot) free_slot = e;
        if (e->used && (int32_t)(e->ms - oldest->ms) < 0) oldest = e;
    }
    return free_slot ? free_slot : oldest;
}

static void on_xfer(ioc_pcache *c, uint16_t id, const uint8_t *data, uint8_t own, uint32_t now) {
    const uint8_t child = (uint8_t)(data[0] >> 5);
    const uint8_t kind = (uint8_t)(data[0] & 0x1Fu);
    const uint8_t transfer = (uint8_t)(data[1] >> 5);
    const uint8_t index = (uint8_t)(data[1] & 0x1Fu);
    /* DTC e confirmações de comando são eventos; os demais tipos, estado. */
    if (kind == IOC_WD_KIND_DTC || kind == IOC_WD_KIND_CMD_ACK) mailbox_push(c, id, data, 8, own, now);
    if (kind == IOC_WD_KIND_CMD_ACK || kind == IOC_WD_KIND_DTC) return;

    ioc_pcache_xfer *slot = NULL;
    ioc_pcache_xfer *free_slot = NULL;
    ioc_pcache_xfer *oldest = NULL;
    for (unsigned i = 0; i < IOC_PCACHE_XFER_SLOTS; ++i) {
        ioc_pcache_xfer *x = &c->xfer[i];
        if (x->used && x->id == id && x->child == child && x->kind == kind) { slot = x; break; }
        if (!x->used) { if (!free_slot) free_slot = x; }
        else if (!oldest || (int32_t)(x->touched_ms - oldest->touched_ms) < 0) oldest = x;
    }
    if (!slot) {
        if (index != 0) return;
        slot = free_slot ? free_slot : oldest;
        memset(slot, 0, sizeof(*slot));
        slot->used = 1;
        slot->id = id;
        slot->child = child;
        slot->kind = kind;
    }
    slot->touched_ms = now;
    slot->own = own;
    if (index == 0) {
        slot->expected = ioc_wd_segment_count(data[2]);
        slot->transfer = transfer;
        slot->building = 0;
        if (slot->expected == 0 || slot->expected > IOC_PCACHE_XFER_FRAMES) { slot->expected = 0; return; }
    } else if (slot->expected == 0 || transfer != slot->transfer || index != slot->building) {
        slot->expected = 0;  /* lacuna: abandona até o próximo início */
        return;
    }
    memcpy(slot->frames[slot->building++], data, 8);
    if (slot->building == slot->expected) {
        memcpy(slot->done, slot->frames, (size_t)slot->expected * 8u);
        slot->committed = slot->expected;
        slot->committed_ms = now;
        slot->expected = 0;
    }
}

void ioc_pcache_on_frame(ioc_pcache *cache, uint16_t id, const uint8_t *data, uint8_t len,
                         uint8_t own, uint32_t now_ms) {
    if (!cache || !data || len > 8) return;
    cache->frames_seen++;
    uint8_t key = 0;
    const ioc_pclass cls = ioc_pcache_classify(id, data, len, &key);
    if (cls == IOC_PCLASS_XFER) {
        on_xfer(cache, id, data, own, now_ms);
        return;
    }
    if (cls == IOC_PCLASS_STATE || cls == IOC_PCLASS_BOTH) {
        ioc_pcache_entry *e = state_slot(cache, id, key);
        /* Mudança do estado de um vínculo também é evento: a caixa postal
         * guarda a transição, o retrato só o último valor. */
        const int assoc = id >= IOC_ID_ASSOC_STATUS_BASE && id <= IOC_ID_ASSOC_STATUS_LAST && ((id - IOC_ID_ASSOC_STATUS_BASE) % 2u) == 0;
        if (assoc && len >= 3 && (!e->used || e->id != id || e->key != key || (e->data[2] & 0x3Fu) != (data[2] & 0x3Fu))) {
            mailbox_push(cache, id, data, len, own, now_ms);
        }
        e->used = 1;
        e->id = id;
        e->key = key;
        e->own = own;
        e->len = len;
        e->ms = now_ms;
        memset(e->data, 0, sizeof(e->data));
        memcpy(e->data, data, len);
    }
    if (cls == IOC_PCLASS_EVENT || cls == IOC_PCLASS_BOTH) mailbox_push(cache, id, data, len, own, now_ms);
}

static void emit_entry(const ioc_pcache_entry *e, uint32_t now, ioc_pcache_emit_fn emit, void *ctx) {
    ioc_panel_frame f;
    f.age_ms = now - e->ms;
    f.id = e->id;
    f.own = e->own;
    f.len = e->len;
    memset(f.data, 0, sizeof(f.data));
    memcpy(f.data, e->data, e->len);
    emit(ctx, &f);
}

void ioc_pcache_snapshot(const ioc_pcache *cache, uint32_t now_ms, ioc_pcache_emit_fn emit, void *ctx) {
    if (!cache || !emit) return;
    /* Estados em ordem de idade, para que o painel processe a história na ordem certa. */
    uint8_t done[IOC_PCACHE_STATE_SLOTS];
    memset(done, 0, sizeof(done));
    for (;;) {
        int best = -1;
        for (unsigned i = 0; i < IOC_PCACHE_STATE_SLOTS; ++i) {
            if (!cache->state[i].used || done[i]) continue;
            if (best < 0 || (int32_t)(cache->state[i].ms - cache->state[best].ms) < 0) best = (int)i;
        }
        if (best < 0) break;
        done[best] = 1;
        emit_entry(&cache->state[best], now_ms, emit, ctx);
    }
    for (unsigned i = 0; i < IOC_PCACHE_XFER_SLOTS; ++i) {
        const ioc_pcache_xfer *x = &cache->xfer[i];
        if (!x->used || x->committed == 0) continue;
        for (uint8_t s = 0; s < x->committed; ++s) {
            ioc_panel_frame f;
            f.age_ms = now_ms - x->committed_ms;
            f.id = x->id;
            f.own = x->own;
            f.len = 8;
            memcpy(f.data, x->done[s], 8);
            emit(ctx, &f);
        }
    }
}

void ioc_pcache_mailbox_open(ioc_pcache *cache, int open) {
    if (!cache) return;
    if (open && !cache->mailbox_open) {
        cache->mb_head = 0;
        cache->mb_count = 0;
        cache->mb_dropped = 0;
    }
    cache->mailbox_open = open ? 1 : 0;
}

void ioc_pcache_mailbox_drain(ioc_pcache *cache, uint32_t now_ms, ioc_pcache_emit_fn emit, void *ctx) {
    if (!cache) return;
    while (cache->mb_count > 0) {
        const ioc_pcache_entry *e = &cache->mailbox[cache->mb_head];
        if (emit) emit_entry(e, now_ms, emit, ctx);
        cache->mb_head = (uint8_t)((cache->mb_head + 1u) % IOC_PCACHE_MAILBOX);
        cache->mb_count--;
    }
}
