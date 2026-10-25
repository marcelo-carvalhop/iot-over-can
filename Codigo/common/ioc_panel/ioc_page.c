#include "ioc_page.h"

#include <stdio.h>
#include <string.h>

#include "ioc_panel_canids.h"

/* ------------------------------------------------------- lista de desenho */

void ioc_dl_clear(ioc_dl *dl) {
    dl->count = 0;
    dl->text_len = 0;
    dl->overflow = 0;
    dl->text[0] = '\0';
}

static ioc_dl_item *push(ioc_dl *dl, uint8_t op, int x, int y, int w, int h, uint8_t ink, uint8_t flags) {
    if (dl->count >= IOC_DL_ITEMS) { dl->overflow = 1; return NULL; }
    ioc_dl_item *it = &dl->item[dl->count++];
    memset(it, 0, sizeof(*it));
    it->op = op;
    it->x = (int16_t)x;
    it->y = (int16_t)y;
    it->w = (int16_t)w;
    it->h = (int16_t)h;
    it->ink = ink;
    it->flags = flags;
    it->text = 0xFFFF;
    return it;
}

void ioc_dl_fill(ioc_dl *dl, int x, int y, int w, int h, uint8_t ink, uint8_t flags) { push(dl, IOC_DL_FILL, x, y, w, h, ink, flags); }
void ioc_dl_frame(ioc_dl *dl, int x, int y, int w, int h, uint8_t ink, uint8_t flags) { push(dl, IOC_DL_FRAME, x, y, w, h, ink, flags); }
void ioc_dl_line(ioc_dl *dl, int x, int y, int dx, int dy, uint8_t ink, uint8_t flags) { push(dl, IOC_DL_LINE, x, y, dx, dy, ink, flags); }

void ioc_dl_symbol(ioc_dl *dl, int x, int y, int size, uint8_t kind, uint8_t ink, uint8_t flags) {
    ioc_dl_item *it = push(dl, IOC_DL_SYMBOL, x, y, kind, size, ink, flags);
    if (it) it->size = (uint8_t)size;
}

static int is_cont(unsigned char c) { return (c & 0xC0u) == 0x80u; }

static size_t utf8_count(const char *s, size_t bytes) {
    size_t n = 0;
    for (size_t i = 0; i < bytes; ++i) if (!is_cont((unsigned char)s[i])) ++n;
    return n;
}

/* Bytes dos primeiros `chars` caracteres. */
static size_t utf8_prefix(const char *s, size_t bytes, size_t chars) {
    size_t i = 0, n = 0;
    while (i < bytes) {
        if (!is_cont((unsigned char)s[i])) {
            if (n == chars) break;
            ++n;
        }
        ++i;
    }
    return i;
}

int ioc_dl_line_height(int size) { return size + 4; }
static int advance(int size) { return size / 2; }
int ioc_dl_text_width(int size, const char *utf8) {
    return utf8 ? (int)utf8_count(utf8, strlen(utf8)) * advance(size) : 0;
}

static int text_bytes(ioc_dl *dl, int x, int y, int size, uint8_t ink, uint8_t flags, const char *s, size_t bytes) {
    if (dl->text_len + bytes + 1 > IOC_DL_TEXT) { dl->overflow = 1; return 0; }
    ioc_dl_item *it = push(dl, IOC_DL_TEXT_OP, x, y, 0, size, ink, flags);
    if (!it) return 0;
    it->size = (uint8_t)size;
    it->text = dl->text_len;
    memcpy(dl->text + dl->text_len, s, bytes);
    dl->text_len = (uint16_t)(dl->text_len + bytes);
    dl->text[dl->text_len++] = '\0';
    it->w = (int16_t)(utf8_count(s, bytes) * (size_t)advance(size));
    return it->w;
}

int ioc_dl_text(ioc_dl *dl, int x, int y, int size, uint8_t ink, uint8_t flags, int w, const char *s) {
    if (!s) return 0;
    size_t bytes = strlen(s);
    if (w > 0) {
        const size_t max_chars = (size_t)(w / advance(size));
        if (utf8_count(s, bytes) > max_chars && max_chars > 3) {
            char tmp[160];
            size_t keep = utf8_prefix(s, bytes, max_chars - 3);
            if (keep > sizeof(tmp) - 4) keep = sizeof(tmp) - 4;
            memcpy(tmp, s, keep);
            memcpy(tmp + keep, "...", 3);
            return text_bytes(dl, x, y, size, ink, flags, tmp, keep + 3);
        }
    }
    return text_bytes(dl, x, y, size, ink, flags, s, bytes);
}

int ioc_dl_wrap(ioc_dl *dl, int x, int y, int w, int size, uint8_t ink, uint8_t flags, const char *s, int max_lines) {
    if (!s || !*s) return 0;
    const size_t max_chars = (size_t)(w / advance(size));
    const size_t total = strlen(s);
    size_t pos = 0;
    int lines = 0;
    while (pos < total && lines < max_lines) {
        while (pos < total && s[pos] == ' ') ++pos;
        if (pos >= total) break;
        const size_t rest = total - pos;
        size_t take = utf8_prefix(s + pos, rest, max_chars);
        if (take < rest) {
            size_t cut = take;
            while (cut > 0 && s[pos + cut] != ' ') --cut;
            if (cut > 0) take = cut;
        }
        const int last = lines == max_lines - 1;
        if (last && pos + take < total) {
            char tmp[160];
            size_t keep = utf8_prefix(s + pos, rest, max_chars > 3 ? max_chars - 3 : max_chars);
            if (keep > sizeof(tmp) - 4) keep = sizeof(tmp) - 4;
            while (keep > 0 && s[pos + keep - 1] == ' ') --keep;
            memcpy(tmp, s + pos, keep);
            memcpy(tmp + keep, "...", 3);
            text_bytes(dl, x, y + lines * ioc_dl_line_height(size), size, ink, flags, tmp, keep + 3);
        } else {
            text_bytes(dl, x, y + lines * ioc_dl_line_height(size), size, ink, flags, s + pos, take);
        }
        pos += take;
        ++lines;
    }
    return lines * ioc_dl_line_height(size);
}

uint32_t ioc_dl_hash(const ioc_dl *dl) {
    uint32_t h = 2166136261u;
#define MIX(b) do { h ^= (uint8_t)(b); h *= 16777619u; } while (0)
    for (uint16_t i = 0; i < dl->count; ++i) {
        const ioc_dl_item *it = &dl->item[i];
        if (it->flags & IOC_DL_VOLATILE) continue;
        MIX(it->op); MIX(it->ink); MIX(it->size);
        MIX(it->x); MIX(it->x >> 8); MIX(it->y); MIX(it->y >> 8);
        MIX(it->w); MIX(it->w >> 8); MIX(it->h); MIX(it->h >> 8);
        if (it->op == IOC_DL_TEXT_OP && it->text < dl->text_len) {
            for (const char *p = dl->text + it->text; *p; ++p) MIX(*p);
        }
    }
#undef MIX
    return h;
}

size_t ioc_dl_serialize(const ioc_dl *dl, char *out, size_t cap) {
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    if (!out || cap == 0) return 0;
    for (uint16_t i = 0; i < dl->count; ++i) {
        const ioc_dl_item *it = &dl->item[i];
        char head[64];
        int len = snprintf(head, sizeof(head), "%s%u,%u,%u,%u,%d,%d,%d,%d,", i ? "|" : "", it->op, it->ink, it->size,
                           it->flags, it->x, it->y, it->w, it->h);
        if (len < 0 || n + (size_t)len + 1 >= cap) break;
        memcpy(out + n, head, (size_t)len);
        n += (size_t)len;
        if (it->op == IOC_DL_TEXT_OP && it->text < dl->text_len) {
            for (const char *p = dl->text + it->text; *p && n + 3 < cap; ++p) {
                out[n++] = hex[((uint8_t)*p) >> 4];
                out[n++] = hex[((uint8_t)*p) & 15u];
            }
        }
    }
    out[n < cap ? n : cap - 1] = '\0';
    return n;
}

/* --------------------------------------------------------------- horários */

void ioc_page_time(const ioc_page_ctx *ctx, uint32_t at_ms, char out[8]) {
    if (ctx->epoch_now > 0) {
        int64_t t = ctx->epoch_now - (int64_t)((int32_t)(ctx->now_ms - at_ms) / 1000) + (int64_t)ctx->tz_min * 60;
        int64_t day = t % 86400;
        if (day < 0) day += 86400;
        char tmp[24];
        snprintf(tmp, sizeof(tmp), "%02d:%02d", (int)(day / 3600), (int)((day / 60) % 60));
        memcpy(out, tmp, 6);
        return;
    }
    int32_t rel = (int32_t)(at_ms - ctx->boot_ms) / 1000;
    if (rel < 0) rel = 0;
    int hours = rel / 3600;
    if (hours > 999) hours = 999;
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "+%d:%02d", hours, (int)((rel / 60) % 60));
    memcpy(out, tmp, 7);
    out[7] = '\0';
}

/* ------------------------------------------------------------- relevância */

static const char *profile_word(uint8_t p) {
    switch (p) {
        case 0x01: return "vibração";
        case 0x02: return "acústico";
        case IOC_PANEL_PROFILE: return "painel";
        default: return "sensor";
    }
}

static int32_t age(const ioc_view *v, uint32_t at) { return (int32_t)(v->now_ms - at); }

static ioc_page_item *add(ioc_page_item *items, int *n, int max, uint8_t level, uint8_t code,
                          uint8_t node, uint8_t child, uint32_t since) {
    if (*n >= max) return NULL;
    ioc_page_item *it = &items[(*n)++];
    memset(it, 0, sizeof(*it));
    it->level = level;
    it->code = code;
    it->node = node;
    it->child = child;
    it->since_ms = since;
    return it;
}

static const char *fault_name(uint8_t kind) {
    switch (kind) {
        case 1: return "queda de energia";
        case 2: return "silêncio";
        case 3: return "queda do ponto de acesso";
        case 4: return "queda de sessão";
        case 5: return "carga no barramento";
        default: return "falha";
    }
}

int ioc_page_assess(const ioc_view *v, const ioc_page_ctx *ctx, ioc_page_item *items, int max) {
    int n = 0;
    char t[8];
    ioc_page_item *it;
    uint32_t hb_limit = v->hb_period_ms * 5u;
    if (hb_limit < 5000u) hb_limit = 5000u;

    if (v->leader && age(v, v->last_heartbeat_ms) > (int32_t)hb_limit) {
        if (v->last_election_ms && age(v, v->last_election_ms) < 15000) {
            if ((it = add(items, &n, max, IOC_LEVEL_HANDLING, IOC_ITEM_ELECTION, 0, 0, v->last_election_ms))) {
                snprintf(it->title, sizeof(it->title), "Eleição em curso");
                snprintf(it->detail, sizeof(it->detail), "O líder (Node %u) deixou de sinalizar; os Nodes escolhem outro.", v->leader);
            }
        } else if ((it = add(items, &n, max, IOC_LEVEL_YOU, IOC_ITEM_NO_LEADER, v->leader, 0, v->last_heartbeat_ms))) {
            ioc_page_time(ctx, v->last_heartbeat_ms, t);
            snprintf(it->title, sizeof(it->title), "Rede sem líder");
            snprintf(it->detail, sizeof(it->detail), "Nenhum sinal de presença desde %s (último líder: Node %u).", t, v->leader);
        }
    } else if (v->prev_leader && v->leader_since_ms && age(v, v->leader_since_ms) < 600000) {
        if ((it = add(items, &n, max, IOC_LEVEL_HANDLING, IOC_ITEM_NEW_LEADER, v->leader, 0, v->leader_since_ms))) {
            ioc_page_time(ctx, v->leader_since_ms, t);
            snprintf(it->title, sizeof(it->title), "Novo líder: Node %u", v->leader);
            snprintf(it->detail, sizeof(it->detail), "Assumiu às %s no lugar do Node %u.", t, v->prev_leader);
        }
    }

    for (uint8_t id = 1; id < IOC_VIEW_NODES; ++id) {
        const ioc_view_node *node = &v->nodes[id];
        if (!node->seen || node->last_ms == 0) continue;
        if (node->absent && id != v->leader) {
            if ((it = add(items, &n, max, IOC_LEVEL_ATTENTION, IOC_ITEM_NODE_ABSENT, id, 0, node->absent_since_ms))) {
                ioc_page_time(ctx, node->absent_since_ms, t);
                snprintf(it->title, sizeof(it->title), "Node %u sem sinal", id);
                snprintf(it->detail, sizeof(it->detail), "Nenhum quadro desde %s.", t);
            }
        }
        const uint8_t joins = ioc_view_joins_within(v, id, 3600000u);
        if (joins >= 3 && (it = add(items, &n, max, IOC_LEVEL_ATTENTION, IOC_ITEM_NODE_REJOINS, id, 0,
                                    node->join_ms[(node->join_n - 1u) % IOC_VIEW_JOINS]))) {
            snprintf(it->title, sizeof(it->title), "Node %u reentrou %u vezes na última hora", id, joins);
            snprintf(it->detail, sizeof(it->detail), "Padrão compatível com queda de alimentação (CH-053).");
        }
    }

    for (unsigned i = 0; i < IOC_VIEW_SENSORS; ++i) {
        const ioc_view_sensor *s = &v->sensors[i];
        if (!s->used) continue;
        const int self = s->node == ctx->serving_node && s->child == ctx->self_child;
        const int asleep_panel = s->profile == IOC_PANEL_PROFILE && s->link_state == 5;
        if (self) continue;
        if (s->sec_since_ms) {
            const unsigned rejected = (unsigned)(s->auth_fail - s->auth_base) + (unsigned)(s->replay - s->replay_base);
            if (rejected && (it = add(items, &n, max, IOC_LEVEL_YOU, IOC_ITEM_SECURITY, s->node, s->child, s->sec_since_ms))) {
                ioc_page_time(ctx, s->sec_since_ms, t);
                snprintf(it->title, sizeof(it->title), "Sensor %02u.%02u: %u datagramas rejeitados", s->node, s->child, rejected);
                snprintf(it->detail, sizeof(it->detail), "Autenticação inválida ou repetição desde %s. Verifique a rede do Node %u.",
                         t, s->node);
            }
        }
        if (s->dtc_count && s->dtc_sev >= 1) {
            if ((it = add(items, &n, max, s->dtc_sev >= 2 ? IOC_LEVEL_YOU : IOC_LEVEL_ATTENTION, IOC_ITEM_DTC,
                          s->node, s->child, s->dtc_ms))) {
                snprintf(it->title, sizeof(it->title), "Diagnóstico 0x%04X em %02u.%02u", s->dtc_code, s->node, s->child);
                snprintf(it->detail, sizeof(it->detail), "Sensor %s, severidade %s.", profile_word(s->profile),
                         s->dtc_sev >= 2 ? "crítica" : "de atenção");
            }
        }
        const int recent_failover = v->failover_ms && age(v, v->failover_ms) < 60000 &&
                                    (s->uuid_parts != 3 || s->uuid == v->failover_uuid);
        if (s->assoc == IOC_ASSOC_LOST || s->assoc == IOC_ASSOC_STALE) {
            const int lost = s->assoc == IOC_ASSOC_LOST;
            if (recent_failover) {
                if ((it = add(items, &n, max, IOC_LEVEL_HANDLING, IOC_ITEM_REASSOC, s->node, s->child, v->failover_ms))) {
                    snprintf(it->title, sizeof(it->title), "Sensor %02u.%02u em reassociação", s->node, s->child);
                    snprintf(it->detail, sizeof(it->detail), "O líder pediu ao Node %u que o assuma.", v->failover_to);
                }
            } else if (!asleep_panel &&
                       (it = add(items, &n, max, lost ? IOC_LEVEL_YOU : IOC_LEVEL_ATTENTION,
                                 lost ? IOC_ITEM_SENSOR_LOST : IOC_ITEM_SENSOR_STALE, s->node, s->child, s->assoc_since_ms))) {
                ioc_page_time(ctx, s->assoc_since_ms, t);
                snprintf(it->title, sizeof(it->title), "%s %02u.%02u %s", s->profile == IOC_PANEL_PROFILE ? "Painel" : "Sensor",
                         s->node, s->child, lost ? "perdido" : "sem sinal recente");
                snprintf(it->detail, sizeof(it->detail), "%s desde %s; o Node %u não o ouve.",
                         lost ? "Perdido" : "Instável", t, s->node);
            }
            continue;
        }
        if ((s->assoc == IOC_ASSOC_ONLINE || s->assoc == IOC_ASSOC_BOUND) && s->link_state != 0xFF &&
            s->link_state != IOC_WD_LINK_SECURE && s->link_state != 5 && age(v, s->link_since_ms) > 30000) {
            if (s->link_state == IOC_WD_LINK_NO_KEY) {
                if ((it = add(items, &n, max, IOC_LEVEL_YOU, IOC_ITEM_NO_KEY, s->node, s->child, s->link_since_ms))) {
                    snprintf(it->title, sizeof(it->title), "Node %u sem chave de enlace", s->node);
                    snprintf(it->detail, sizeof(it->detail), "%02u.%02u não terá dados: provisione e regrave o Node.", s->node, s->child);
                }
            } else if ((it = add(items, &n, max, IOC_LEVEL_ATTENTION, IOC_ITEM_NO_SESSION, s->node, s->child, s->link_since_ms))) {
                ioc_page_time(ctx, s->link_since_ms, t);
                snprintf(it->title, sizeof(it->title), "Sensor %02u.%02u sem sessão de dados", s->node, s->child);
                snprintf(it->detail, sizeof(it->detail), s->link_state == IOC_WD_LINK_HANDSHAKE
                             ? "A autenticação com o Node %u não conclui desde %s."
                             : "O Node %u anuncia a oferta desde %s; o sensor não entrou.", s->node, t);
            }
        }
    }

    if (v->fault_active) {
        if ((it = add(items, &n, max, IOC_LEVEL_HANDLING, IOC_ITEM_FAULT_TEST, v->fault_node, 0, v->fault_since_ms))) {
            snprintf(it->title, sizeof(it->title), "Ensaio de %s em andamento", fault_name(v->fault_kind));
            if (v->fault_node) snprintf(it->detail, sizeof(it->detail), "Alvo: Node %u. A rede deve se recuperar sozinha.", v->fault_node);
            else snprintf(it->detail, sizeof(it->detail), "Gerado pela Probe 00.");
        }
    } else if (v->load_pct >= 70.0f) {
        const int high = v->load_pct >= 90.0f;
        if ((it = add(items, &n, max, high ? IOC_LEVEL_YOU : IOC_LEVEL_ATTENTION, IOC_ITEM_BUS_LOAD, 0, 0, v->now_ms))) {
            snprintf(it->title, sizeof(it->title), "Barramento %s", high ? "saturado" : "muito ocupado");
            snprintf(it->detail, sizeof(it->detail), "Ocupação acima de %d%%; mensagens de baixa prioridade atrasam.", high ? 90 : 70);
        }
    }

    if (!ctx->usb_power && ctx->battery_pct != 255 && ctx->battery_pct < 15) {
        if ((it = add(items, &n, max, IOC_LEVEL_ATTENTION, IOC_ITEM_PANEL_BATTERY, 0, 0, ctx->now_ms))) {
            snprintf(it->title, sizeof(it->title), "Bateria do painel abaixo de 15%%");
            snprintf(it->detail, sizeof(it->detail), "Ligue o painel à USB; sem energia ele mostra a última página.");
        }
    }

    /* Ordem: nível decrescente, depois o mais antigo primeiro. */
    for (int i = 1; i < n; ++i) {
        ioc_page_item tmp = items[i];
        int j = i - 1;
        while (j >= 0 && (items[j].level < tmp.level ||
                          (items[j].level == tmp.level && (int32_t)(items[j].since_ms - tmp.since_ms) > 0))) {
            items[j + 1] = items[j];
            --j;
        }
        items[j + 1] = tmp;
    }
    return n;
}

uint8_t ioc_page_level(const ioc_page_item *items, int n) {
    uint8_t level = IOC_LEVEL_NORMAL;
    for (int i = 0; i < n; ++i) if (items[i].level > level) level = items[i].level;
    return level;
}

const char *ioc_page_level_title(uint8_t level) {
    switch (level) {
        case IOC_LEVEL_HANDLING: return "A rede está tratando";
        case IOC_LEVEL_ATTENTION: return "Atenção";
        case IOC_LEVEL_YOU: return "Requer você";
        default: return "Rede normal";
    }
}

/* -------------------------------------------------------------- composição */

static const uint8_t LEVEL_INK[4] = {IOC_INK_BLACK, IOC_INK_BLUE, IOC_INK_YELLOW, IOC_INK_RED};
static const uint8_t LEVEL_FG[4] = {IOC_INK_WHITE, IOC_INK_WHITE, IOC_INK_BLACK, IOC_INK_WHITE};
static const uint8_t LEVEL_SYM[4] = {IOC_SYM_CHECK, IOC_SYM_DOT, IOC_SYM_TRIANGLE, IOC_SYM_CROSS};
/* Texto sobre o papel: amarelo não é legível, então "atenção" usa preto. */
static const uint8_t LEVEL_TEXT[4] = {IOC_INK_BLACK, IOC_INK_BLUE, IOC_INK_BLACK, IOC_INK_RED};

#define FOOTER_Y 574
#define MARGIN 8

int ioc_page_header(ioc_dl *dl, uint8_t level, const char *title, const char *right) {
    if (level > 3) level = 3;
    ioc_dl_fill(dl, 0, 0, IOC_DL_W, 48, LEVEL_INK[level], 0);
    ioc_dl_symbol(dl, 12, 12, 24, LEVEL_SYM[level], LEVEL_FG[level], 0);
    const int rw = right ? ioc_dl_text_width(16, right) : 0;
    ioc_dl_text(dl, 46, 12, 24, LEVEL_FG[level], 0, IOC_DL_W - 60 - rw, title);
    if (right && *right) ioc_dl_text(dl, IOC_DL_W - MARGIN - rw, 18, 16, LEVEL_FG[level], 0, 0, right);
    return 56;
}

int ioc_page_footer(ioc_dl *dl, const ioc_page_ctx *ctx) {
    char until[8], line[96], bat[16] = "", env[24] = "";
    ioc_page_time(ctx, ctx->now_ms + ctx->valid_for_s * 1000u, until);
    if (ctx->usb_power) snprintf(bat, sizeof(bat), " · USB");
    else if (ctx->battery_pct != 255) snprintf(bat, sizeof(bat), " · bat %u%%", ctx->battery_pct);
    if (ctx->temp_c10 != IOC_PANEL_NA_I16) {
        snprintf(env, sizeof(env), " · %d°C", (ctx->temp_c10 + (ctx->temp_c10 >= 0 ? 5 : -5)) / 10);
        if (ctx->rh_10 != IOC_PANEL_NA_U16) {
            const size_t used = strlen(env);
            snprintf(env + used, sizeof(env) - used, " %u%%", (unsigned)((ctx->rh_10 + 5u) / 10u));
        }
    }
    if (ctx->serving_node) snprintf(line, sizeof(line), "Válido até %s · Node %u%s%s", until, ctx->serving_node, bat, env);
    else snprintf(line, sizeof(line), "Válido até %s%s%s", until, bat, env);
    ioc_dl_line(dl, 0, FOOTER_Y, IOC_DL_W, 0, IOC_INK_BLACK, 0);
    ioc_dl_text(dl, MARGIN, FOOTER_Y + 5, 16, IOC_INK_BLACK, IOC_DL_VOLATILE, IOC_DL_W - 2 * MARGIN, line);
    return FOOTER_Y;
}

static const char *node_word(const ioc_view *v, uint8_t id) {
    const ioc_view_node *n = &v->nodes[id];
    if (n->absent) return "fora";
    if (id == v->leader) return "líder";
    switch (n->status) {
        case IOC_ST_FOLLOWER_OK: return "normal";
        case IOC_ST_DISABLED: return "desativ.";
        case IOC_ST_FAULT: return "falha";
        case IOC_ST_JOINING: return "entrando";
        case IOC_ST_CONFLICT: return "conflito";
        case IOC_ST_LEADER: return "líder";
        default: return "ativo";
    }
}

static int node_grid(ioc_dl *dl, const ioc_view *v, int y, int max_rows) {
    const int cw = 92, ch = 46, gap = 4;
    int col = 0, row = 0, shown = 0, total = 0;
    for (uint8_t id = 1; id < IOC_VIEW_NODES; ++id) {
        const ioc_view_node *n = &v->nodes[id];
        if (!n->seen || n->last_ms == 0) continue;
        ++total;
        if (row >= max_rows) continue;
        const int x = MARGIN + col * (cw + gap), yy = y + row * (ch + gap);
        const uint8_t ink = n->absent ? IOC_INK_RED : IOC_INK_BLACK;
        ioc_dl_frame(dl, x, yy, cw, ch, ink, 0);
        if (n->absent) ioc_dl_frame(dl, x + 1, yy + 1, cw - 2, ch - 2, ink, 0);
        char num[8];
        snprintf(num, sizeof(num), "%u", id);
        ioc_dl_text(dl, x + 6, yy + 3, 24, ink, 0, 0, num);
        if (id == v->leader) ioc_dl_symbol(dl, x + cw - 22, yy + 6, 16, IOC_SYM_STAR, IOC_INK_BLACK, 0);
        ioc_dl_text(dl, x + 6, yy + 27, 16, ink, 0, cw - 8, node_word(v, id));
        ++shown;
        if (++col == 4) { col = 0; ++row; }
    }
    if (total == 0) {
        ioc_dl_text(dl, MARGIN, y, 16, IOC_INK_BLACK, 0, 0, "Nenhum Node observado ainda.");
        return 24;
    }
    int rows = (shown + 3) / 4;
    if (total > shown) {
        char more[48];
        snprintf(more, sizeof(more), "+%d Nodes na página Rede", total - shown);
        ioc_dl_text(dl, MARGIN, y + rows * (ch + gap), 16, IOC_INK_BLACK, 0, 0, more);
        return rows * (ch + gap) + 22;
    }
    return rows * (ch + gap);
}

static void sensor_value(const ioc_view_sensor *s, char *out, size_t cap) {
    if (s->profile == IOC_PANEL_PROFILE) {
        if (s->battery_pct != 255) snprintf(out, cap, "bat %u%%", s->battery_pct);
        else out[0] = '\0';
    } else if (!s->have_value) {
        snprintf(out, cap, "sem leitura");
    } else if (s->profile == 0x02) {
        snprintf(out, cap, "%d dB(A) · dose %d%%", (int)(s->value + 0.5f), (int)(s->value2 + 0.5f));
    } else {
        const int centi = (int)(s->value * 100.0f + 0.5f);
        snprintf(out, cap, "RMS %d,%02d m/s²", centi / 100, centi % 100);
    }
}

static const char *assoc_word(uint8_t state) {
    switch (state) {
        case IOC_ASSOC_ONLINE: return "online";
        case IOC_ASSOC_BOUND: return "associado";
        case IOC_ASSOC_ASSOCIATING: return "associando";
        case IOC_ASSOC_STALE: return "instável";
        case IOC_ASSOC_LOST: return "perdido";
        default: return "?";
    }
}

static const char *link_word(uint8_t state) {
    switch (state) {
        case IOC_WD_LINK_SECURE: return "sessão";
        case IOC_WD_LINK_OFFERING: return "oferta";
        case IOC_WD_LINK_HANDSHAKE: return "autenticando";
        case IOC_WD_LINK_NO_KEY: return "sem chave";
        case 5: return "dormindo";
        case IOC_WD_LINK_DOWN: return "sem dados";
        default: return "";
    }
}

static int sensors_block(ioc_dl *dl, const ioc_view *v, const ioc_page_ctx *ctx, int y, int limit_y, int detailed) {
    int y0 = y;
    int count = 0;
    for (unsigned i = 0; i < IOC_VIEW_SENSORS; ++i) count += v->sensors[i].used ? 1 : 0;
    if (count == 0 || y + 44 > limit_y) return 0;
    char t[8], head[48];
    ioc_page_time(ctx, v->now_ms, t);
    snprintf(head, sizeof(head), "Leituras às %s", t);
    ioc_dl_text(dl, MARGIN, y, 16, IOC_INK_BLACK, IOC_DL_VOLATILE, 0, head);
    y += 22;
    for (unsigned i = 0; i < IOC_VIEW_SENSORS && y + 20 <= limit_y; ++i) {
        const ioc_view_sensor *s = &v->sensors[i];
        if (!s->used) continue;
        char id[72], val[64];
        if (detailed) {
            snprintf(id, sizeof(id), "%02u.%02u %s, %s %s", s->node, s->child, profile_word(s->profile),
                     assoc_word(s->assoc), link_word(s->link_state));
            ioc_dl_text(dl, MARGIN, y, 16, IOC_INK_BLACK, 0, IOC_DL_W - 2 * MARGIN, id);
            y += 20;
            if (y + 20 > limit_y) break;
            sensor_value(s, val, sizeof(val));
            ioc_dl_text(dl, MARGIN + 48, y, 16, IOC_INK_BLACK, IOC_DL_VOLATILE, IOC_DL_W - 56 - MARGIN, val);
        } else {
            snprintf(id, sizeof(id), "%02u.%02u %s", s->node, s->child, profile_word(s->profile));
            const int w = ioc_dl_text(dl, MARGIN, y, 16, IOC_INK_BLACK, 0, 0, id);
            sensor_value(s, val, sizeof(val));
            ioc_dl_text(dl, MARGIN + w + 8, y, 16, IOC_INK_BLACK, IOC_DL_VOLATILE, IOC_DL_W - 2 * MARGIN - w - 8, val);
        }
        y += 20;
    }
    return y - y0 + 4;
}

static int bus_block(ioc_dl *dl, const ioc_view *v, int y, int limit_y) {
    if (y + 46 > limit_y || (v->load_n == 0 && v->bin_n == 0 && v->load_pct <= 0.0f)) return 0;
    char line[64];
    snprintf(line, sizeof(line), "Barramento %d%% · média %d%% · pico %d%%", (int)(v->load_pct + 0.5f),
             (int)(ioc_view_load_avg(v) + 0.5f), (int)(v->load_peak_pct + 0.5f));
    ioc_dl_text(dl, MARGIN, y, 16, IOC_INK_BLACK, IOC_DL_VOLATILE, IOC_DL_W - 2 * MARGIN, line);
    const int gy = y + 22, gh = 20, gw = IOC_DL_W - 2 * MARGIN;
    ioc_dl_line(dl, MARGIN, gy + gh, gw, 0, IOC_INK_BLACK, IOC_DL_VOLATILE);
    if (v->load_n >= 2) {
        float top = 10.0f;
        for (uint8_t i = 0; i < v->load_n; ++i) if (v->load_hist[i] / 2.0f > top) top = v->load_hist[i] / 2.0f;
        const int step = gw / (IOC_VIEW_LOAD_BINS - 1);
        for (uint8_t i = 1; i < v->load_n; ++i) {
            const int x0 = MARGIN + (i - 1) * step;
            const int y0 = gy + gh - (int)((v->load_hist[i - 1] / 2.0f) / top * gh);
            const int y1 = gy + gh - (int)((v->load_hist[i] / 2.0f) / top * gh);
            ioc_dl_line(dl, x0, y0, step, y1 - y0, IOC_INK_BLACK, IOC_DL_VOLATILE);
        }
    }
    return 48;
}

static int marks_block(ioc_dl *dl, const ioc_page_ctx *ctx, int y, int limit_y) {
    int used = 0;
    if (!ctx->marks[0] || y + 42 > limit_y) return 0;
    ioc_dl_text(dl, MARGIN, y, 16, IOC_INK_BLACK, 0, 0, "Suas marcas");
    used += 20;
    for (int i = 0; i < IOC_PAGE_MARKS && ctx->marks[i] && y + used + 20 <= limit_y; ++i) {
        ioc_dl_text(dl, MARGIN + 8, y + used, 16, IOC_INK_BLACK, 0, IOC_DL_W - 2 * MARGIN - 8, ctx->marks[i]);
        used += 20;
    }
    return used + 4;
}

static uint32_t normal_since(const ioc_view *v) {
    uint32_t since_ms = v->started_ms;
    for (uint8_t i = 0; i < v->ev_count; ++i) {
        const ioc_view_event *e = ioc_view_event_at(v, i);
        if (e && e->kind != IOC_VEV_JOIN && (int32_t)(e->ms - since_ms) > 0) since_ms = e->ms;
    }
    return since_ms;
}

void ioc_page_situation(ioc_dl *dl, const ioc_view *v, const ioc_page_ctx *ctx, const ioc_page_item *items, int n) {
    ioc_dl_clear(dl);
    const uint8_t level = ioc_page_level(items, n);
    char right[32] = "";
    if (n) snprintf(right, sizeof(right), n == 1 ? "1 item" : "%d itens", n);
    int y = ioc_page_header(dl, level, ioc_page_level_title(level), right);
    const int footer = FOOTER_Y - 4;
    if (n == 0) {
        char t[8], line[64];
        ioc_dl_symbol(dl, MARGIN + 4, y + 8, 40, IOC_SYM_CHECK, IOC_INK_BLACK, 0);
        ioc_dl_text(dl, 64, y + 6, 32, IOC_INK_BLACK, 0, 0, "Rede normal");
        ioc_page_time(ctx, ctx->normal_since_ms ? ctx->normal_since_ms : normal_since(v), t);
        snprintf(line, sizeof(line), "sem intercorrências desde %s", t);
        ioc_dl_text(dl, 64, y + 44, 16, IOC_INK_BLACK, 0, 0, line);
        y += 72;
        uint8_t absent = 0;
        const uint8_t nodes = ioc_view_node_count(v, &absent);
        if (v->leader) snprintf(line, sizeof(line), "Líder: Node %u · %u Nodes · %u filhos", v->leader, nodes, ioc_view_sensor_count(v));
        else snprintf(line, sizeof(line), "%u Nodes · %u filhos", nodes, ioc_view_sensor_count(v));
        ioc_dl_text(dl, MARGIN, y, 16, IOC_INK_BLACK, 0, 0, line);
        y += 28;
    } else {
        int shown = 0;
        for (int i = 0; i < n && y + 64 < 360; ++i) {
            const ioc_page_item *it = &items[i];
            const uint8_t ink = LEVEL_TEXT[it->level];
            const uint8_t bar = LEVEL_INK[it->level];
            const int top = y;
            ioc_dl_symbol(dl, 20, y + 3, 20, LEVEL_SYM[it->level], ink, 0);
            y += ioc_dl_wrap(dl, 48, y, IOC_DL_W - 48 - MARGIN, 24, ink, 0, it->title, 2);
            y += ioc_dl_wrap(dl, 48, y, IOC_DL_W - 48 - MARGIN, 16, IOC_INK_BLACK, 0, it->detail, 2);
            ioc_dl_fill(dl, MARGIN, top, 5, y - top, bar, 0);
            y += 10;
            ++shown;
        }
        if (shown < n) {
            char more[48];
            snprintf(more, sizeof(more), "+%d na página Rede (botão B)", n - shown);
            ioc_dl_text(dl, 48, y, 16, IOC_INK_BLACK, 0, 0, more);
            y += 22;
        }
    }
    ioc_dl_line(dl, MARGIN, y, IOC_DL_W - 2 * MARGIN, 0, IOC_INK_BLACK, 0);
    y += 6;
    y += node_grid(dl, v, y, 2) + 8;
    y += sensors_block(dl, v, ctx, y, footer - 70, 0);
    y += bus_block(dl, v, y, footer - 30);
    marks_block(dl, ctx, y, footer);
    ioc_page_footer(dl, ctx);
}

static const char *assoc_event_word(uint8_t state) {
    switch (state) {
        case IOC_ASSOC_ONLINE: return "voltou";
        case IOC_ASSOC_STALE: return "instável";
        case IOC_ASSOC_LOST: return "perdido";
        case IOC_ASSOC_UNBOUND: return "desassociado";
        case IOC_ASSOC_REJECTED: return "recusado";
        default: return "mudou";
    }
}

static void event_text(const ioc_view_event *e, char *out, size_t cap) {
    switch (e->kind) {
        case IOC_VEV_LEADER: snprintf(out, cap, "Líder: Node %u (antes %u)", e->node, e->arg); break;
        case IOC_VEV_ELECTION: snprintf(out, cap, "Eleição pedida"); break;
        case IOC_VEV_JOIN: snprintf(out, cap, "Node %u entrou na rede", e->node); break;
        case IOC_VEV_FAILOVER: snprintf(out, cap, "Reassociação para o Node %u", e->node); break;
        case IOC_VEV_ASSOC: snprintf(out, cap, "%02u.%02u %s", e->node, e->child, assoc_event_word(e->arg)); break;
        case IOC_VEV_FAULT: snprintf(out, cap, "Ensaio %s (Node %u)", e->child == IOC_TEST_STARTED ? "iniciado" : "encerrado", e->node); break;
        case IOC_VEV_DTC: snprintf(out, cap, "Diagnóstico em %02u.%02u", e->node, e->child); break;
        case IOC_VEV_NODE_LOST: snprintf(out, cap, "Node %u sem sinal", e->node); break;
        case IOC_VEV_NODE_BACK: snprintf(out, cap, "Node %u voltou", e->node); break;
        default: snprintf(out, cap, "Evento"); break;
    }
}

void ioc_page_network(ioc_dl *dl, const ioc_view *v, const ioc_page_ctx *ctx, const ioc_page_item *items, int n) {
    ioc_dl_clear(dl);
    const uint8_t level = ioc_page_level(items, n);
    char right[32] = "";
    uint8_t absent = 0;
    const uint8_t nodes = ioc_view_node_count(v, &absent);
    snprintf(right, sizeof(right), "%u Nodes", nodes);
    int y = ioc_page_header(dl, level, "Rede", right);
    y += node_grid(dl, v, y, 4) + 8;
    y += sensors_block(dl, v, ctx, y, 400, 1);
    ioc_dl_line(dl, MARGIN, y, IOC_DL_W - 2 * MARGIN, 0, IOC_INK_BLACK, 0);
    y += 6;
    if (v->ev_count) {
        ioc_dl_text(dl, MARGIN, y, 16, IOC_INK_BLACK, 0, 0, "Acontecimentos recentes");
        y += 22;
        for (uint8_t i = 0; i < v->ev_count && y + 20 <= FOOTER_Y - 54; ++i) {
            const ioc_view_event *e = ioc_view_event_at(v, i);
            char t[8], text[64], line[80];
            ioc_page_time(ctx, e->ms, t);
            event_text(e, text, sizeof(text));
            snprintf(line, sizeof(line), "%s %s", t, text);
            ioc_dl_text(dl, MARGIN, y, 16, IOC_INK_BLACK, 0, IOC_DL_W - 2 * MARGIN, line);
            y += 20;
        }
    }
    bus_block(dl, v, FOOTER_Y - 52, FOOTER_Y - 2);
    ioc_page_footer(dl, ctx);
}

/* ------------------------------------------------- política de atualização */

int ioc_refresh_decide(ioc_refresh *r, uint32_t now, uint32_t hash, uint8_t level, int forced, uint32_t *next_ms) {
    uint32_t next = 0;
    int now_draw = 0;
    if (!r->have || forced) {
        now_draw = 1;
    } else if (hash == r->hash && level == r->level) {
        r->pending_since_ms = 0;
        const uint32_t renew = r->valid_until_ms - IOC_REFRESH_RENEW_MS;
        if ((int32_t)(now - renew) >= 0) now_draw = 1;
        else next = renew;
    } else if (level > r->level) {
        now_draw = 1;
    } else if (level < r->level) {
        if (r->pending_since_ms == 0 || r->pending_hash != hash) {
            r->pending_since_ms = now;
            r->pending_hash = hash;
        }
        if (now - r->pending_since_ms >= IOC_REFRESH_DEESC_MS) now_draw = 1;
        else next = r->pending_since_ms + IOC_REFRESH_DEESC_MS;
    } else {
        r->pending_since_ms = 0;
        if (now - r->last_ms >= IOC_REFRESH_MIN_MS) now_draw = 1;
        else next = r->last_ms + IOC_REFRESH_MIN_MS;
    }
    if (next_ms) *next_ms = next;
    return now_draw;
}

void ioc_refresh_done(ioc_refresh *r, uint32_t now, uint32_t hash, uint8_t level, uint32_t valid_until_ms) {
    r->have = 1;
    r->hash = hash;
    r->level = level;
    r->last_ms = now;
    r->valid_until_ms = valid_until_ms;
    r->pending_since_ms = 0;
    r->count++;
}
