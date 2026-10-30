/* Painel em papel eletrônico: formatos, retrato do Node, visão da rede, página e política. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ioc_page.h"
#include "ioc_panel_cache.h"
#include "ioc_panel_canids.h"
#include "ioc_panel_proto.h"
#include "ioc_view.h"
#include "ioc_wdata.h"

#include "../../node-can/include/can_ids.h"
#include "../../node-can/include/protocolo.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

/* Os valores copiados para o painel conferem com os do firmware do Node. */
static void test_ids(void) {
    CHECK(IOC_ID_ELECTION == CAN_ID_ELECTION);
    CHECK(IOC_ID_LEADER == CAN_ID_LEADER);
    CHECK(IOC_ID_CONTROL == CAN_ID_CONTROL);
    CHECK(IOC_ID_HEARTBEAT == CAN_ID_HEARTBEAT);
    CHECK(IOC_ID_SENSOR == CAN_ID_SENSOR);
    CHECK(IOC_ID_DISCOVERY_BASE == CAN_ID_WIRELESS_DISCOVERY_BASE);
    CHECK(IOC_ID_ASSOC_STATUS_BASE == CAN_ID_WIRELESS_ASSOC_STATUS_BASE);
    CHECK(IOC_ID_DATA_BASE == CAN_ID_WIRELESS_DATA_BASE);
    CHECK(IOC_ID_DATA_CMD == CAN_ID_WIRELESS_DATA_CMD);
    CHECK(IOC_ID_TEST_STATUS == CAN_ID_TEST_STATUS);
    CHECK(IOC_ID_LOAD_LOW == CAN_ID_TEST_LOAD_LOW);
    CHECK(IOC_SUB_JOIN_REQUEST == CTRL_SUBCMD_JOIN_REQUEST);
    CHECK(IOC_SUB_STATE_UPDATE == CTRL_SUBCMD_STATE_UPDATE);
    CHECK(IOC_ASSOC_LOST == WIRELESS_ASSOC_LOST);
    CHECK(IOC_ASSOC_FAILOVER_OWNER == WIRELESS_ASSOC_ACTION_FAILOVER_OWNER_LOST);
    CHECK(IOC_ST_FOLLOWER_OK == STATUS_FOLLOWER_OK);
    CHECK(IOC_PANEL_WD_KIND == 8);
}

static void test_proto(void) {
    CHECK(ioc_panel_age_decode(ioc_panel_age_encode(1234)) == 1234);
    CHECK(ioc_panel_age_decode(ioc_panel_age_encode(90000)) == 90000);
    CHECK(ioc_panel_age_decode(ioc_panel_age_encode(0xFFFFFFFFu)) == 32767000u);

    uint8_t buf[IOC_PANEL_DATAGRAM_MAX];
    ioc_panel_frames_hdr h = {IOC_PANEL_FR_LIVE, 7, 250, 1000, 0};
    size_t used = ioc_panel_frames_begin(buf, sizeof(buf), &h);
    CHECK(used == IOC_PANEL_FRAMES_HEADER);
    ioc_panel_frame f = {12, 0x100, 1, 5, {1, 3, 9, 4, 3, 0, 0, 0}};
    used = ioc_panel_frames_add(buf, sizeof(buf), used, &f);
    CHECK(used == IOC_PANEL_FRAMES_HEADER + 10);
    size_t off = 0;
    ioc_panel_frames_hdr r;
    CHECK(ioc_panel_frames_parse(buf, used, &r, &off) == 1);
    CHECK(r.count == 1 && r.seq == 7 && r.span_ms == 250 && r.skipped_bits == 1000);
    ioc_panel_frame g;
    CHECK(ioc_panel_frames_next(buf, used, &off, &g) == 1);
    CHECK(g.id == 0x100 && g.own == 1 && g.len == 5 && g.data[2] == 9 && g.age_ms == 12);
    CHECK(ioc_panel_frames_next(buf, used, &off, &g) == 0);
    CHECK(ioc_panel_frames_next(buf, used - 1, &(size_t){IOC_PANEL_FRAMES_HEADER}, &g) == -1);
    /* Não ultrapassa a capacidade. */
    size_t small = ioc_panel_frames_begin(buf, 20, &h);
    CHECK(ioc_panel_frames_add(buf, 20, small, &f) == 0);

    ioc_panel_status st = {IOC_PANEL_ST_USB | IOC_PANEL_ST_CLOCK, 81, 3950, 241, 512, 300, 0xDEADBEEF, 9, 40, 3600, 2};
    uint8_t body[IOC_PANEL_STATUS_BODY];
    ioc_panel_status_encode_body(&st, body);
    ioc_panel_status st2;
    CHECK(ioc_panel_status_decode_body(body, sizeof(body), &st2) == 1);
    CHECK(st2.page_hash == 0xDEADBEEF && st2.temp_c10 == 241 && st2.severity == 2 && st2.uptime_s == 3600);

    ioc_panel_mark m = {5, IOC_PANEL_MARK_NOTE, 0, 1500, 0, ""};
    strcpy(m.text, "troquei o cabo");
    m.text_len = (uint8_t)strlen(m.text);
    uint8_t mb[128];
    size_t mlen = ioc_panel_mark_encode_body(&m, mb, sizeof(mb));
    ioc_panel_mark m2;
    CHECK(ioc_panel_mark_decode_body(mb, mlen, &m2) == 1);
    CHECK(m2.seq == 5 && m2.age_ms == 1500 && strcmp(m2.text, "troquei o cabo") == 0);
    CHECK(ioc_panel_mark_decode_body(mb, mlen - 1, &m2) == 0);

    uint8_t msg[64];
    size_t n = ioc_panel_msg(IOC_PANEL_CMD_MARK, mb, mlen, msg, sizeof(msg));
    const uint8_t *b;
    size_t bl;
    CHECK(ioc_panel_msg_parse(msg, n, &b, &bl) == IOC_PANEL_CMD_MARK && bl == mlen);
    ioc_panel_hello hello = {IOC_PANEL_VERSION, IOC_PANEL_HELLO_SNAPSHOT, 300}, hello2;
    uint8_t hb[IOC_PANEL_HELLO_LEN];
    ioc_panel_hello_encode(&hello, hb);
    CHECK(ioc_panel_hello_decode(hb, sizeof(hb), &hello2) && hello2.sleep_s == 300);
}

/* --------------------------------------------------- tráfego de uma rede */

static uint32_t now = 100000;

typedef struct { void (*sink)(uint16_t, const uint8_t *, uint8_t); } bus_t;

static ioc_pcache cache;
static void to_cache(uint16_t id, const uint8_t *d, uint8_t len) { ioc_pcache_on_frame(&cache, id, d, len, 0, now); }

static void heartbeat(void (*sink)(uint16_t, const uint8_t *, uint8_t), uint8_t leader) {
    uint8_t d[5] = {0x01, leader, 1, 3, 3};
    sink(0x100, d, 5);
}

static void follower(void (*sink)(uint16_t, const uint8_t *, uint8_t), uint8_t node) {
    uint8_t d[5] = {node, 1, 0xAA, 1, 0};
    sink(0x200, d, 5);
}

static void assoc(void (*sink)(uint16_t, const uint8_t *, uint8_t), uint8_t node, uint8_t child, uint8_t state,
                  uint8_t profile, uint64_t uuid) {
    uint8_t a[8] = {1, child, state, profile, (uint8_t)(uuid >> 56), (uint8_t)(uuid >> 48), (uint8_t)(uuid >> 40), (uint8_t)(uuid >> 32)};
    uint8_t b[8] = {1, child, (uint8_t)-55, 6, (uint8_t)(uuid >> 24), (uint8_t)(uuid >> 16), (uint8_t)(uuid >> 8), (uint8_t)uuid};
    sink((uint16_t)(0x320 + 2 * node), a, 8);
    sink((uint16_t)(0x320 + 2 * node + 1), b, 8);
}

static void transfer(void (*sink)(uint16_t, const uint8_t *, uint8_t), uint8_t node, uint8_t child, uint8_t kind,
                     const uint8_t *payload, size_t len) {
    static uint8_t counter;
    const uint8_t segs = ioc_wd_segment_count(len);
    for (uint8_t i = 0; i < segs; ++i) {
        uint8_t frame[8];
        ioc_wd_build_segment(child, kind, counter, payload, len, i, frame);
        sink((uint16_t)(0x380 + node), frame, 8);
    }
    counter = (uint8_t)((counter + 1u) & 7u);
}

static void telemetry(void (*sink)(uint16_t, const uint8_t *, uint8_t), uint8_t node, uint8_t child, float rms) {
    ioc_wd_telemetry t;
    memset(&t, 0, sizeof(t));
    t.rms = rms;
    t.kurtosis = 3.1f;
    t.battery_pct = 90;
    uint8_t raw[IOC_WD_TELEMETRY_LEN];
    ioc_wd_telemetry_encode(&t, raw);
    transfer(sink, node, child, IOC_WD_KIND_TELEMETRY, raw, sizeof(raw));
}

static void link_report(void (*sink)(uint16_t, const uint8_t *, uint8_t), uint8_t node, uint8_t child, uint8_t state,
                        uint16_t auth_fail) {
    ioc_wd_link l;
    memset(&l, 0, sizeof(l));
    l.state = state;
    l.auth_failures = auth_fail;
    l.rtt_100us = 40;
    l.rtt_max_100us = 60;
    uint8_t raw[IOC_WD_LINK_LEN];
    ioc_wd_link_encode(&l, raw);
    transfer(sink, node, child, IOC_WD_KIND_LINK, raw, sizeof(raw));
}

static uint8_t batch[IOC_PANEL_DATAGRAM_MAX];
static size_t batch_used;
static int batches_sent;
static ioc_view view;

static void flush_batch(uint8_t flags) {
    if (batch_used <= IOC_PANEL_FRAMES_HEADER && !(flags & IOC_PANEL_FR_END)) return;
    ioc_panel_frames_set_flags(batch, flags);
    CHECK(ioc_view_batch(&view, batch, batch_used, now) >= 0);
    batches_sent++;
    ioc_panel_frames_hdr h = {0, (uint16_t)batches_sent, 0, 0, 0};
    batch_used = ioc_panel_frames_begin(batch, sizeof(batch), &h);
}

static void emit_to_batch(void *ctx, const ioc_panel_frame *f) {
    (void)ctx;
    size_t next = ioc_panel_frames_add(batch, sizeof(batch), batch_used, f);
    if (!next) {
        flush_batch(IOC_PANEL_FR_SNAPSHOT);
        next = ioc_panel_frames_add(batch, sizeof(batch), batch_used, f);
    }
    batch_used = next;
}

static void to_view(uint16_t id, const uint8_t *d, uint8_t len) { ioc_view_frame(&view, id, d, len, now); }

static void test_cache_and_view(void) {
    ioc_pcache_init(&cache);
    /* Rede: líder 3, seguidores 1, 2 e 4; sensor 02.01 com sessão; painel 04.01. */
    for (int round = 0; round < 3; ++round) {
        heartbeat(to_cache, 3);
        follower(to_cache, 1);
        follower(to_cache, 2);
        follower(to_cache, 4);
        now += 1000;
    }
    assoc(to_cache, 2, 1, 0x04, 0x01, 0xE6616408432B6F39ull);
    assoc(to_cache, 4, 1, 0x04, IOC_PANEL_PROFILE, 0x50C0000000000001ull);
    telemetry(to_cache, 2, 1, 0.13f);
    link_report(to_cache, 2, 1, IOC_WD_LINK_SECURE, 0);
    { uint8_t spec[8] = {(uint8_t)((1u << 5) | IOC_WD_KIND_SPECTRUM), 0, 20, 0, 0, 0, 0, 0}; to_cache(0x382, spec, 8); }
    { uint8_t load[8] = {0}; to_cache(0x010, load, 8); }

    CHECK(ioc_pcache_forward_live(0x010, (const uint8_t *)"\0\0\0\0\0\0\0\0", 8) == 0);
    CHECK(ioc_pcache_forward_live(0x100, (const uint8_t *)"\1\3\0\0\0", 5) == 1);
    {   /* etapas do canal de dados (v0.22): diagnóstico para a TUI, fora do painel */
        const uint8_t ev[8] = {(uint8_t)((1u << 5) | IOC_WD_KIND_LINK_EVENT), 0, IOC_WD_LINK_EVENT_LEN, 0, 1, 0, 1, 0};
        const uint8_t tel[8] = {(uint8_t)((1u << 5) | IOC_WD_KIND_TELEMETRY), 0, IOC_WD_TELEMETRY_LEN, 0, 0, 0, 0, 0};
        CHECK(ioc_pcache_forward_live(0x382, ev, 8) == 0);
        CHECK(ioc_pcache_forward_live(0x382, tel, 8) == 1);
    }

    /* O painel dorme: a caixa postal abre e guarda a eleição e o vínculo que muda. */
    ioc_pcache_mailbox_open(&cache, 1);
    { uint8_t e[1] = {4}; to_cache(0x050, e, 1); }
    assoc(to_cache, 2, 1, 0x05, 0x01, 0xE6616408432B6F39ull);  /* STALE */
    heartbeat(to_cache, 3);                                     /* estado, não evento */
    CHECK(cache.mb_count == 2);

    /* O painel acorda: retrato + caixa postal, entregues como lotes. */
    ioc_view_init(&view, now);
    ioc_view_set_self(&view, 4, 1);
    ioc_panel_frames_hdr h = {0, 0, 0, 0, 0};
    batch_used = ioc_panel_frames_begin(batch, sizeof(batch), &h);
    ioc_pcache_snapshot(&cache, now, emit_to_batch, NULL);
    flush_batch(IOC_PANEL_FR_SNAPSHOT);
    ioc_pcache_mailbox_drain(&cache, now, emit_to_batch, NULL);
    flush_batch(IOC_PANEL_FR_MAILBOX | IOC_PANEL_FR_END);
    CHECK(cache.mb_count == 0);
    CHECK(view.snapshot_done == 1);
    CHECK(view.leader == 3);
    uint8_t absent = 9;
    CHECK(ioc_view_node_count(&view, &absent) == 4 && absent == 0);
    CHECK(ioc_view_sensor_count(&view) == 2);
    int found = 0;
    for (unsigned i = 0; i < IOC_VIEW_SENSORS; ++i) {
        const ioc_view_sensor *s = &view.sensors[i];
        if (s->used && s->node == 2 && s->child == 1) {
            found = 1;
            CHECK(s->uuid == 0xE6616408432B6F39ull);
            CHECK(s->have_value && s->value > 0.12f && s->value < 0.14f);
            CHECK(s->link_state == IOC_WD_LINK_SECURE);
            CHECK(s->assoc == 0x05);
        }
    }
    CHECK(found);

    /* Página: o sensor instável é o item de atenção; o painel não se lista. */
    ioc_page_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.now_ms = now;
    ctx.epoch_now = 1761400000;  /* 2025-10-25 13:46:40 UTC */
    ctx.tz_min = -180;
    ctx.battery_pct = 80;
    ctx.temp_c10 = 241;
    ctx.rh_10 = 512;
    ctx.valid_for_s = 3600;
    ctx.serving_node = 4;
    ctx.self_child = 1;
    char t[8];
    ioc_page_time(&ctx, now, t);
    CHECK(strcmp(t, "10:46") == 0);
    ioc_page_item items[IOC_PAGE_MAX_ITEMS];
    int n = ioc_page_assess(&view, &ctx, items, IOC_PAGE_MAX_ITEMS);
    CHECK(n == 1 && items[0].code == IOC_ITEM_SENSOR_STALE && items[0].level == IOC_LEVEL_ATTENTION);

    static ioc_dl a, b;
    ioc_page_situation(&a, &view, &ctx, items, n);
    CHECK(!a.overflow && a.count > 10);
    for (uint16_t i = 0; i < a.count; ++i) {
        const ioc_dl_item *it = &a.item[i];
        if (it->op == IOC_DL_TEXT_OP) CHECK(it->x >= 0 && it->x + it->w <= IOC_DL_W);
        CHECK(it->y >= 0 && it->y < IOC_DL_H);
    }
    /* Leituras e rodapé mudam sem mudar o hash; a ausência de um Node muda. */
    now += 20000;
    for (int round = 0; round < 3; ++round) {
        heartbeat(to_view, 3);
        follower(to_view, 1);
        follower(to_view, 2);
        follower(to_view, 4);
        now += 1000;
    }
    telemetry(to_view, 2, 1, 0.21f);
    ctx.now_ms = now;
    ctx.epoch_now += 23;
    ioc_view_tick(&view, now);
    n = ioc_page_assess(&view, &ctx, items, IOC_PAGE_MAX_ITEMS);
    ioc_page_situation(&b, &view, &ctx, items, n);
    CHECK(ioc_dl_hash(&a) == ioc_dl_hash(&b));

    for (int round = 0; round < 8; ++round) {  /* Node 1 para de transmitir */
        heartbeat(to_view, 3);
        follower(to_view, 2);
        follower(to_view, 4);
        now += 1000;
    }
    ioc_view_tick(&view, now);
    ctx.now_ms = now;
    n = ioc_page_assess(&view, &ctx, items, IOC_PAGE_MAX_ITEMS);
    CHECK(n == 2 && ioc_page_level(items, n) == IOC_LEVEL_ATTENTION);
    ioc_page_situation(&b, &view, &ctx, items, n);
    CHECK(ioc_dl_hash(&a) != ioc_dl_hash(&b));

    /* Contadores de rejeição: item "requer você". */
    link_report(to_view, 2, 1, IOC_WD_LINK_SECURE, 14);
    n = ioc_page_assess(&view, &ctx, items, IOC_PAGE_MAX_ITEMS);
    CHECK(items[0].code == IOC_ITEM_SECURITY && items[0].level == IOC_LEVEL_YOU);

    /* Reassociação pedida pelo líder: o sensor perdido vira "a rede está tratando". */
    assoc(to_view, 2, 1, 0x06, 0x01, 0xE6616408432B6F39ull);
    uint8_t fa[8] = {9, 4, 0x11, 0xE6, 0x61, 0x64, 0x08, 0x43};
    uint8_t fb[8] = {9, 4, 0x11, 0x2B, 0x6F, 0x39, 2, (uint8_t)-60};
    to_view(0x302, fa, 8);
    to_view(0x303, fb, 8);
    n = ioc_page_assess(&view, &ctx, items, IOC_PAGE_MAX_ITEMS);
    int reassoc = 0;
    for (int i = 0; i < n; ++i) reassoc |= items[i].code == IOC_ITEM_REASSOC && items[i].level == IOC_LEVEL_HANDLING;
    CHECK(reassoc);

    /* Rede: página de detalhe e serialização. */
    ioc_page_network(&b, &view, &ctx, items, n);
    CHECK(!b.overflow);
    static char ser[16384];
    CHECK(ioc_dl_serialize(&b, ser, sizeof(ser)) > 100);
    CHECK(strchr(ser, '|') != NULL);
}

static void test_dl(void) {
    static ioc_dl dl;
    ioc_dl_clear(&dl);
    CHECK(ioc_dl_text_width(16, "ação") == 32);
    const int h = ioc_dl_wrap(&dl, 0, 0, 80, 16, IOC_INK_BLACK, 0, "uma frase longa que precisa quebrar em linhas", 2);
    CHECK(h == 40 && dl.count == 2);
    CHECK(strstr(dl.text + dl.item[1].text, "...") != NULL);
    ioc_dl_text(&dl, 0, 0, 16, IOC_INK_BLACK, 0, 40, "corte necessário");
    CHECK(ioc_dl_text_width(16, dl.text + dl.item[2].text) <= 40);
}

static void test_refresh(void) {
    ioc_refresh r;
    memset(&r, 0, sizeof(r));
    uint32_t next = 0;
    CHECK(ioc_refresh_decide(&r, 0, 1, 0, 0, &next) == 1);
    ioc_refresh_done(&r, 0, 1, 0, 3600000);
    /* Igual: só renova a validade perto do fim. */
    CHECK(ioc_refresh_decide(&r, 1000, 1, 0, 0, &next) == 0 && next == 3600000 - IOC_REFRESH_RENEW_MS);
    CHECK(ioc_refresh_decide(&r, 3400000, 1, 0, 0, &next) == 1);
    /* Agravamento: imediato. */
    CHECK(ioc_refresh_decide(&r, 2000, 2, 3, 0, &next) == 1);
    ioc_refresh_done(&r, 2000, 2, 3, 3600000);
    /* Mesmo nível, outro conteúdo: intervalo mínimo. */
    CHECK(ioc_refresh_decide(&r, 30000, 3, 3, 0, &next) == 0 && next == 2000 + IOC_REFRESH_MIN_MS);
    CHECK(ioc_refresh_decide(&r, 2000 + IOC_REFRESH_MIN_MS, 3, 3, 0, &next) == 1);
    /* Melhora: espera o conteúdo ficar estável. */
    CHECK(ioc_refresh_decide(&r, 10000, 4, 0, 0, &next) == 0);
    CHECK(ioc_refresh_decide(&r, 10000 + IOC_REFRESH_DEESC_MS - 1, 4, 0, 0, &next) == 0);
    CHECK(ioc_refresh_decide(&r, 10000 + IOC_REFRESH_DEESC_MS, 4, 0, 0, &next) == 1);
    /* Oscilação reinicia a espera. */
    ioc_refresh_done(&r, 0, 2, 3, 3600000);
    CHECK(ioc_refresh_decide(&r, 10000, 4, 0, 0, &next) == 0);
    CHECK(ioc_refresh_decide(&r, 100000, 5, 0, 0, &next) == 0);
    CHECK(ioc_refresh_decide(&r, 10000 + IOC_REFRESH_DEESC_MS, 5, 0, 0, &next) == 0);
    CHECK(ioc_refresh_decide(&r, 50000, 9, 3, 1, &next) == 1);  /* pedido do operador */
}

/* Marca IoT-over-CAN: dentro da largura pedida, com o nome, sem estourar a lista. */
static void test_logo(void) {
    static ioc_dl dl;
    ioc_dl_clear(&dl);
    const int h = ioc_page_logo(&dl, 8, 28, 384);
    CHECK(h > 100 && h < 200);
    CHECK(!dl.overflow && dl.count > 10);
    int named = 0;
    for (uint16_t i = 0; i < dl.count; ++i) {
        const ioc_dl_item *it = &dl.item[i];
        CHECK(it->x >= 8 && it->x + (it->op == IOC_DL_TEXT_OP ? ioc_dl_text_width(it->size, dl.text + it->text) : it->w) <= 392);
        CHECK(!(it->flags & IOC_DL_VOLATILE));
        if (it->op == IOC_DL_TEXT_OP && strcmp(dl.text + it->text, "IoT-over-") == 0) named = 1;
    }
    CHECK(named);
    /* Os sinalizadores de marca e estado são distintos e nomeados. */
    CHECK(strcmp(ioc_panel_mark_kind_name(IOC_PANEL_MARK_FLAG), "FLAG") == 0);
    CHECK(strcmp(ioc_panel_mark_kind_name(IOC_PANEL_MARK_ACK), "ACK") == 0);
    CHECK((IOC_PANEL_ST_ALERT & (IOC_PANEL_ST_USB | IOC_PANEL_ST_SLEEP | IOC_PANEL_ST_POWER_OFF |
                                 IOC_PANEL_ST_CLOCK | IOC_PANEL_ST_KEYBOARD)) == 0);
}

int main(void) {
    test_logo();
    test_ids();
    test_proto();
    test_dl();
    test_cache_and_view();
    test_refresh();
    if (failures) {
        fprintf(stderr, "test_ioc_panel: %d falha(s)\n", failures);
        return 1;
    }
    puts("test_ioc_panel: PASS");
    return 0;
}
