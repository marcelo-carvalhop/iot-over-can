#include "ioc_view.h"

#include <string.h>

#include "ioc_canbits.h"
#include "ioc_panel_canids.h"

static int32_t since(uint32_t now, uint32_t then) { return (int32_t)(now - then); }

void ioc_view_init(ioc_view *v, uint32_t now_ms) {
    memset(v, 0, sizeof(*v));
    v->now_ms = now_ms;
    v->started_ms = now_ms;
    v->hb_period_ms = 1000;
    v->bin_start_ms = now_ms;
    for (unsigned i = 0; i < IOC_VIEW_NODES; ++i) {
        v->nodes[i].status = 0xFF;
        ioc_wd_rx_reset(&v->rx[i]);
    }
}

void ioc_view_set_self(ioc_view *v, uint8_t node, uint8_t child) {
    v->self_node = node;
    v->self_child = child;
}

static void event(ioc_view *v, uint32_t at, uint8_t kind, uint8_t node, uint8_t child, uint8_t arg) {
    ioc_view_event *e = &v->events[(v->ev_head + v->ev_count) % IOC_VIEW_EVENTS];
    if (v->ev_count == IOC_VIEW_EVENTS) {
        v->ev_head = (uint8_t)((v->ev_head + 1u) % IOC_VIEW_EVENTS);
        e = &v->events[(v->ev_head + v->ev_count - 1u) % IOC_VIEW_EVENTS];
    } else {
        v->ev_count++;
    }
    e->ms = at;
    e->kind = kind;
    e->node = node;
    e->child = child;
    e->arg = arg;
}

const ioc_view_event *ioc_view_event_at(const ioc_view *v, uint8_t newest_index) {
    if (newest_index >= v->ev_count) return NULL;
    return &v->events[(v->ev_head + v->ev_count - 1u - newest_index) % IOC_VIEW_EVENTS];
}

static void touch_node(ioc_view *v, uint8_t id, uint32_t at) {
    if (id == 0 || id >= IOC_VIEW_NODES) return;
    ioc_view_node *n = &v->nodes[id];
    if (!n->seen) { n->seen = 1; n->first_ms = at; }
    if (since(at, n->last_ms) > 0 || n->last_ms == 0) n->last_ms = at;
    if (n->absent && since(at, n->absent_since_ms) >= 0) {
        n->absent = 0;
        event(v, at, IOC_VEV_NODE_BACK, id, 0, 0);
    }
}

static void set_leader(ioc_view *v, uint8_t id, uint32_t at) {
    if (id == 0 || id >= IOC_VIEW_NODES) return;
    if (v->leader != id) {
        if (v->leader != 0) event(v, at, IOC_VEV_LEADER, id, 0, v->leader);
        v->prev_leader = v->leader;
        v->leader = id;
        v->leader_since_ms = at;
    }
}

static ioc_view_sensor *sensor_at(ioc_view *v, uint8_t node, uint8_t child, int create) {
    ioc_view_sensor *free_slot = NULL;
    for (unsigned i = 0; i < IOC_VIEW_SENSORS; ++i) {
        ioc_view_sensor *s = &v->sensors[i];
        if (s->used && s->node == node && s->child == child) return s;
        if (!s->used && !free_slot) free_slot = s;
    }
    if (!create || !free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = 1;
    free_slot->node = node;
    free_slot->child = child;
    free_slot->link_state = 0xFF;
    free_slot->battery_pct = 255;
    free_slot->rtt_100us = IOC_WD_RTT_UNKNOWN;
    return free_slot;
}

static uint64_t uuid_hi(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32);
}

static uint64_t uuid_lo(const uint8_t *p) {
    return ((uint64_t)p[0] << 24) | ((uint64_t)p[1] << 16) | ((uint64_t)p[2] << 8) | (uint64_t)p[3];
}

/* Mesmo empacotamento de ioc_assoc_pack (ioc_failover.c): estado em 6 bits,
 * AUTO no bit 7, sessão no bit 6; REJECTED (0x7F) não leva sinalizadores. */
static uint8_t assoc_state(uint8_t raw) { return raw == IOC_ASSOC_REJECTED ? raw : (uint8_t)(raw & 0x3Fu); }

static void on_assoc(ioc_view *v, uint16_t id, const uint8_t *d, uint8_t len, uint32_t at) {
    if (len < 8) return;
    const uint8_t node = (uint8_t)((id - IOC_ID_ASSOC_STATUS_BASE) / 2u);
    const int part_a = ((id - IOC_ID_ASSOC_STATUS_BASE) % 2u) == 0;
    const uint8_t child = d[1];
    touch_node(v, node, at);
    if (child == 0) return;
    const uint8_t raw = d[2];
    const uint8_t state_a = assoc_state(raw);
    if (part_a && (state_a == IOC_ASSOC_UNBOUND || state_a == IOC_ASSOC_REJECTED)) {
        ioc_view_sensor *gone = sensor_at(v, node, child, 0);
        if (gone) {
            event(v, at, IOC_VEV_ASSOC, node, child, state_a);
            gone->used = 0;
        }
        return;
    }
    ioc_view_sensor *s = sensor_at(v, node, child, 1);
    if (!s) return;
    if (since(at, s->last_ms) > 0 || s->last_ms == 0) s->last_ms = at;
    if (part_a) {
        const uint8_t state = state_a;
        s->auto_failover = raw != IOC_ASSOC_REJECTED && (raw & 0x80u) ? 1 : 0;
        s->secure_flag = raw != IOC_ASSOC_REJECTED && (raw & 0x40u) ? 1 : 0;
        if (state != s->assoc) {
            const uint8_t before = s->assoc;
            s->assoc = state;
            s->assoc_since_ms = at;
            if (state == IOC_ASSOC_STALE || state == IOC_ASSOC_LOST ||
                (state == IOC_ASSOC_ONLINE && (before == IOC_ASSOC_LOST || before == IOC_ASSOC_STALE))) {
                event(v, at, IOC_VEV_ASSOC, node, child, state);
            }
        }
        if (d[3]) s->profile = d[3];
        s->uuid = (s->uuid & 0x00000000FFFFFFFFull) | uuid_hi(&d[4]);
        s->uuid_parts |= 1;
    } else {
        s->rssi = (int8_t)d[2];
        s->uuid = (s->uuid & 0xFFFFFFFF00000000ull) | uuid_lo(&d[4]);
        s->uuid_parts |= 2;
    }
    /* Um mesmo sensor em dois Nodes: vale o vínculo mais recente (reassociação). */
    if (s->uuid_parts == 3) {
        for (unsigned i = 0; i < IOC_VIEW_SENSORS; ++i) {
            ioc_view_sensor *o = &v->sensors[i];
            if (o == s || !o->used || o->uuid_parts != 3 || o->uuid != s->uuid) continue;
            if (since(s->assoc_since_ms, o->assoc_since_ms) >= 0 && o->assoc != IOC_ASSOC_ONLINE) o->used = 0;
        }
    }
}

static void security_counters(ioc_view_sensor *s, uint16_t auth, uint16_t replay, uint32_t at) {
    if (!s->sec_base_set) {
        s->sec_base_set = 1;
        s->auth_base = auth;
        s->replay_base = replay;
    }
    if (auth < s->auth_base) s->auth_base = auth;      /* sessão nova zera os contadores */
    if (replay < s->replay_base) s->replay_base = replay;
    if ((auth > s->auth_base || replay > s->replay_base) && s->sec_since_ms == 0) s->sec_since_ms = at;
    s->auth_fail = auth;
    s->replay = replay;
}

static void on_transfer(ioc_view *v, uint8_t node, const ioc_wd_rx_slot *slot, uint32_t at) {
    ioc_view_sensor *s = sensor_at(v, node, slot->child, slot->kind != IOC_PANEL_WD_KIND);
    switch (slot->kind) {
        case IOC_WD_KIND_TELEMETRY: {
            ioc_wd_telemetry t;
            if (!s || !ioc_wd_telemetry_decode(slot->data, slot->total_len, &t)) return;
            s->have_value = 1;
            s->value = t.rms;
            s->value2 = t.kurtosis;
            s->value_ms = at;
            s->dtc_count = t.dtc_count;
            s->dtc_code = t.dtc_code;
            s->battery_pct = t.battery_pct;
            if (!s->profile) s->profile = 0x01;
            break;
        }
        case IOC_WD_KIND_ACOUSTIC: {
            ioc_wd_acoustic a;
            if (!s || !ioc_wd_acoustic_decode(slot->data, slot->total_len, &a)) return;
            s->have_value = a.laeq_cb != IOC_WD_CB_NA;
            s->value = (float)a.laeq_cb / 100.0f;
            s->value2 = (float)a.dose_nr15_x10 / 10.0f;
            s->value_ms = at;
            s->dtc_count = a.dtc_count;
            s->dtc_code = a.dtc_code;
            s->battery_pct = a.battery_pct;
            s->profile = 0x02;
            break;
        }
        case IOC_WD_KIND_LINK: {
            ioc_wd_link l;
            if (!s || !ioc_wd_link_decode(slot->data, slot->total_len, &l)) return;
            if (l.state != s->link_state) {
                s->link_state = l.state;
                s->link_since_ms = at;
            }
            security_counters(s, l.auth_failures, l.replay_drops, at);
            s->lost = l.lost_datagrams;
            s->rtt_100us = l.rtt_100us;
            break;
        }
        case IOC_WD_KIND_CONFIG: {
            ioc_wd_config c;
            if (s && ioc_wd_config_decode(slot->data, slot->total_len, &c) && c.profile) s->profile = c.profile;
            break;
        }
        case IOC_WD_KIND_DTC: {
            ioc_wd_dtc d;
            if (!s || !ioc_wd_dtc_decode(slot->data, slot->total_len, &d)) return;
            s->dtc_code = d.code;
            s->dtc_sev = d.severity;
            s->dtc_ms = at;
            if (s->dtc_count == 0) s->dtc_count = 1;
            event(v, at, IOC_VEV_DTC, node, slot->child, d.severity);
            break;
        }
        case IOC_PANEL_WD_KIND: {
            if (!s || slot->total_len < 1 || slot->data[0] != IOC_PANEL_CMD_STATUS) return;
            ioc_panel_status st;
            if (ioc_panel_status_decode_body(slot->data + 1, (size_t)slot->total_len - 1u, &st)) {
                s->panel_flags = st.flags;
                s->profile = IOC_PANEL_PROFILE;
                s->battery_pct = st.battery_pct;
            }
            break;
        }
        default:
            break;
    }
    if (s) s->last_ms = at;
}

void ioc_view_frame(ioc_view *v, uint16_t id, const uint8_t *d, uint8_t len, uint32_t at) {
    if (!v || !d || len > 8) return;
    v->frames++;
    if (id == IOC_ID_HEARTBEAT && len >= 4 && d[0] == 0x01) {
        const uint8_t leader = d[1];
        if (v->last_heartbeat_ms && v->leader == leader) {
            const int32_t dt = since(at, v->last_heartbeat_ms);
            if (dt >= 100 && dt <= 5000) v->hb_period_ms = (v->hb_period_ms * 3u + (uint32_t)dt) / 4u;
        }
        if (since(at, v->last_heartbeat_ms) > 0 || v->last_heartbeat_ms == 0) v->last_heartbeat_ms = at;
        touch_node(v, leader, at);
        set_leader(v, leader, at);
    } else if (id == IOC_ID_LEADER && len >= 1) {
        touch_node(v, d[0], at);
        set_leader(v, d[0], at);
    } else if (id == IOC_ID_ELECTION && len >= 1) {
        if (v->last_election_ms == 0 || since(at, v->last_election_ms) > 5000) event(v, at, IOC_VEV_ELECTION, d[0], 0, 0);
        v->last_election_ms = at;
        touch_node(v, d[0], at);
    } else if (id == IOC_ID_SENSOR && len >= 1) {
        touch_node(v, d[0], at);
    } else if (id == IOC_ID_CONTROL && len >= 4) {
        if (d[0] == IOC_STATUS_OPCODE &&
            (d[1] == IOC_SUB_STATE_UPDATE || d[1] == IOC_SUB_STATE_FORCE || d[1] == IOC_SUB_STATUS_RESP)) {
            if (d[2] > 0 && d[2] < IOC_VIEW_NODES) {
                v->nodes[d[2]].seen = 1;
                v->nodes[d[2]].status = d[3];
            }
        } else if (d[0] == IOC_CTRL_OPCODE && d[1] == IOC_SUB_JOIN_REQUEST && d[2] > 0 && d[2] < IOC_VIEW_NODES) {
            ioc_view_node *n = &v->nodes[d[2]];
            n->join_ms[n->join_n % IOC_VIEW_JOINS] = at;
            n->join_n++;
            touch_node(v, d[2], at);
            event(v, at, IOC_VEV_JOIN, d[2], 0, 0);
        }
    } else if (id >= IOC_ID_DISCOVERY_BASE && id <= IOC_ID_DISCOVERY_LAST) {
        touch_node(v, (uint8_t)((id - IOC_ID_DISCOVERY_BASE) / 2u), at);
    } else if ((id == 0x302 || id == 0x303) && len >= 8) {
        if (id == 0x302) {
            v->failover_seq = d[0];
            memcpy(v->failover_uuid_a, &d[3], 5);
        } else if (d[0] == v->failover_seq &&
                   (d[2] == IOC_ASSOC_FAILOVER_OWNER || d[2] == IOC_ASSOC_FAILOVER_SENSOR)) {
            uint64_t u = 0;
            for (int i = 0; i < 5; ++i) u = (u << 8) | v->failover_uuid_a[i];
            for (int i = 3; i < 6; ++i) u = (u << 8) | d[i];
            v->failover_uuid = u;
            v->failover_to = d[1];
            v->failover_reason = d[2];
            v->failover_ms = at;
            event(v, at, IOC_VEV_FAILOVER, d[1], 0, d[2]);
        }
    } else if (id == IOC_ID_TEST_STATUS && len >= 8 && d[7] == 0x5A) {
        if (d[3] == IOC_TEST_STARTED) {
            v->fault_active = 1;
            v->fault_kind = d[2];
            v->fault_node = d[1];
            v->fault_since_ms = at;
        } else {
            v->fault_active = 0;
        }
        event(v, at, IOC_VEV_FAULT, d[1], d[3], d[2]);
    } else if (id >= IOC_ID_ASSOC_STATUS_BASE && id <= IOC_ID_ASSOC_STATUS_LAST) {
        on_assoc(v, id, d, len, at);
    } else if (id > IOC_ID_DATA_BASE && id <= IOC_ID_DATA_LAST && len == 8) {
        const uint8_t node = (uint8_t)(id - IOC_ID_DATA_BASE);
        touch_node(v, node, at);
        const ioc_wd_rx_slot *done = NULL;
        if (ioc_wd_rx_push(&v->rx[node], d, at, &done) == IOC_WD_RX_COMPLETE && done) on_transfer(v, node, done, at);
    }
}

static void load_sample(ioc_view *v, float pct, uint32_t now) {
    v->load_pct = pct;
    if (pct > v->load_peak_pct) v->load_peak_pct = pct;
    v->bin_sum += pct;
    v->bin_n++;
    if (since(now, v->bin_start_ms) >= (int32_t)IOC_VIEW_LOAD_BIN_MS) {
        float avg = v->bin_sum / (float)v->bin_n;
        if (avg > 100.0f) avg = 100.0f;
        if (v->load_n == IOC_VIEW_LOAD_BINS) {
            memmove(v->load_hist, v->load_hist + 1, IOC_VIEW_LOAD_BINS - 1);
            v->load_n--;
        }
        v->load_hist[v->load_n++] = (uint8_t)(avg * 2.0f + 0.5f);
        v->bin_sum = 0.0f;
        v->bin_n = 0;
        v->bin_start_ms = now;
    }
}

int ioc_view_batch(ioc_view *v, const uint8_t *buf, size_t len, uint32_t now_ms) {
    ioc_panel_frames_hdr hdr;
    size_t off = 0;
    if (!ioc_panel_frames_parse(buf, len, &hdr, &off)) return -1;
    v->now_ms = now_ms;
    v->batches++;
    v->last_batch_ms = now_ms;
    if ((hdr.flags & IOC_PANEL_FR_LIVE) && v->have_seq && (uint16_t)(v->last_seq + 1u) != hdr.seq) v->batch_gaps++;
    v->last_seq = hdr.seq;
    v->have_seq = 1;
    int n = 0;
    uint32_t bits = hdr.skipped_bits;
    ioc_panel_frame f;
    int r;
    while ((r = ioc_panel_frames_next(buf, len, &off, &f)) == 1) {
        ioc_view_frame(v, f.id, f.data, f.len, now_ms - f.age_ms);
        if (hdr.flags & IOC_PANEL_FR_LIVE) bits += ioc_can_frame_bits(f.id, f.len, f.data);
        ++n;
    }
    if (r < 0) return -1;
    if (hdr.flags & IOC_PANEL_FR_END) v->snapshot_done = 1;
    if ((hdr.flags & IOC_PANEL_FR_LIVE) && hdr.span_ms > 0) {
        v->win_bits += bits;
        v->win_span_ms += hdr.span_ms;
        if (v->win_span_ms >= 1000u) {
            const float pct = 100.0f * (float)v->win_bits / (500.0f * (float)v->win_span_ms);
            load_sample(v, pct, now_ms);
            v->win_bits = 0;
            v->win_span_ms = 0;
        }
    }
    return n;
}

void ioc_view_tick(ioc_view *v, uint32_t now_ms) {
    v->now_ms = now_ms;
    uint32_t limit = v->hb_period_ms * 6u;
    if (limit < 6000u) limit = 6000u;
    for (uint8_t id = 1; id < IOC_VIEW_NODES; ++id) {
        ioc_view_node *n = &v->nodes[id];
        if (!n->seen || n->last_ms == 0) continue;
        if (!n->absent && since(now_ms, n->last_ms) > (int32_t)limit) {
            n->absent = 1;
            n->absent_since_ms = n->last_ms;
            event(v, n->last_ms + limit, IOC_VEV_NODE_LOST, id, 0, 0);
        }
    }
}

uint8_t ioc_view_node_count(const ioc_view *v, uint8_t *absent) {
    uint8_t total = 0, away = 0;
    for (uint8_t id = 1; id < IOC_VIEW_NODES; ++id) {
        if (!v->nodes[id].seen || v->nodes[id].last_ms == 0) continue;
        total++;
        if (v->nodes[id].absent) away++;
    }
    if (absent) *absent = away;
    return total;
}

uint8_t ioc_view_sensor_count(const ioc_view *v) {
    uint8_t n = 0;
    for (unsigned i = 0; i < IOC_VIEW_SENSORS; ++i) n += v->sensors[i].used ? 1u : 0u;
    return n;
}

uint8_t ioc_view_joins_within(const ioc_view *v, uint8_t node, uint32_t window_ms) {
    if (node == 0 || node >= IOC_VIEW_NODES) return 0;
    const ioc_view_node *n = &v->nodes[node];
    uint8_t count = 0;
    const uint8_t stored = n->join_n < IOC_VIEW_JOINS ? n->join_n : IOC_VIEW_JOINS;
    for (uint8_t i = 0; i < stored; ++i) {
        if (since(v->now_ms, n->join_ms[i]) <= (int32_t)window_ms) count++;
    }
    return count;
}

float ioc_view_load_avg(const ioc_view *v) {
    if (v->load_n == 0) return v->bin_n ? v->bin_sum / (float)v->bin_n : v->load_pct;
    uint32_t sum = 0;
    for (uint8_t i = 0; i < v->load_n; ++i) sum += v->load_hist[i];
    return (float)sum / (2.0f * (float)v->load_n);
}
