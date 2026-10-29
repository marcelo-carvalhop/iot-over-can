/**
 * @file ioc_page.h
 * @brief Relevância, composição da página e política de atualização do papel.
 *
 * A página é uma lista de desenho (retângulos, linhas, símbolos e texto) e não
 * uma imagem: o firmware a desenha com o M5GFX, o Studio com um canvas, e os
 * testes a examinam diretamente. O texto usa avanço fixo de metade da altura
 * (a fonte efont do M5GFX), o que permite quebrar linhas aqui mesmo.
 *
 * A atualização é por significado: o hash da página ignora os itens marcados
 * como voláteis (leituras, ocupação, rodapé), de modo que só uma mudança do
 * que o operador precisa saber provoca um redesenho de 15 a 30 s.
 */
#ifndef IOC_PAGE_H
#define IOC_PAGE_H

#include <stddef.h>
#include <stdint.h>

#include "ioc_view.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IOC_DL_W      400
#define IOC_DL_H      600
#define IOC_DL_ITEMS  220
#define IOC_DL_TEXT   4096

/* As seis tintas do Spectra 6. */
enum { IOC_INK_BLACK = 0, IOC_INK_WHITE = 1, IOC_INK_YELLOW = 2, IOC_INK_RED = 3, IOC_INK_BLUE = 4, IOC_INK_GREEN = 5 };
enum { IOC_DL_FILL = 1, IOC_DL_FRAME = 2, IOC_DL_TEXT_OP = 3, IOC_DL_LINE = 4, IOC_DL_SYMBOL = 5 };
enum { IOC_SYM_DOT = 0, IOC_SYM_TRIANGLE = 1, IOC_SYM_CROSS = 2, IOC_SYM_CHECK = 3, IOC_SYM_STAR = 4 };

#define IOC_DL_VOLATILE 0x01   /* fora do hash: não provoca redesenho */

typedef struct {
    uint8_t  op;
    uint8_t  ink;
    uint8_t  size;      /* texto: 16, 24 ou 32 px; símbolo: lado em px */
    uint8_t  flags;
    int16_t  x, y, w, h; /* linha: w,h = deslocamento; símbolo: w = tipo */
    uint16_t text;      /* deslocamento em text[] */
} ioc_dl_item;

typedef struct {
    ioc_dl_item item[IOC_DL_ITEMS];
    uint16_t count;
    uint16_t text_len;
    uint8_t  overflow;
    char     text[IOC_DL_TEXT];
} ioc_dl;

void     ioc_dl_clear(ioc_dl *dl);
void     ioc_dl_fill(ioc_dl *dl, int x, int y, int w, int h, uint8_t ink, uint8_t flags);
void     ioc_dl_frame(ioc_dl *dl, int x, int y, int w, int h, uint8_t ink, uint8_t flags);
void     ioc_dl_line(ioc_dl *dl, int x, int y, int dx, int dy, uint8_t ink, uint8_t flags);
void     ioc_dl_symbol(ioc_dl *dl, int x, int y, int size, uint8_t kind, uint8_t ink, uint8_t flags);
/* Uma linha; corta o texto que não couber em w (0 = sem limite). Devolve a largura. */
int      ioc_dl_text(ioc_dl *dl, int x, int y, int size, uint8_t ink, uint8_t flags, int w, const char *utf8);
/* Quebra em palavras; devolve a altura usada. */
int      ioc_dl_wrap(ioc_dl *dl, int x, int y, int w, int size, uint8_t ink, uint8_t flags,
                     const char *utf8, int max_lines);
int      ioc_dl_text_width(int size, const char *utf8);
int      ioc_dl_line_height(int size);
uint32_t ioc_dl_hash(const ioc_dl *dl);
/* Texto para a bancada e o Studio: itens "op,tinta,tam,flags,x,y,w,h,texto-hex" separados por "|". */
size_t   ioc_dl_serialize(const ioc_dl *dl, char *out, size_t cap);

/* ------------------------------------------------------------- relevância */

enum { IOC_LEVEL_NORMAL = 0, IOC_LEVEL_HANDLING = 1, IOC_LEVEL_ATTENTION = 2, IOC_LEVEL_YOU = 3 };

enum {
    IOC_ITEM_NO_LEADER = 1, IOC_ITEM_ELECTION, IOC_ITEM_NEW_LEADER, IOC_ITEM_NODE_ABSENT,
    IOC_ITEM_NODE_REJOINS, IOC_ITEM_SENSOR_LOST, IOC_ITEM_SENSOR_STALE, IOC_ITEM_REASSOC,
    IOC_ITEM_NO_SESSION, IOC_ITEM_NO_KEY, IOC_ITEM_SECURITY, IOC_ITEM_DTC, IOC_ITEM_BUS_LOAD,
    IOC_ITEM_FAULT_TEST, IOC_ITEM_PANEL_BATTERY, IOC_ITEM_ORPHAN
};

#define IOC_PAGE_MAX_ITEMS 12

typedef struct {
    uint8_t  level;
    uint8_t  code;
    uint8_t  node;
    uint8_t  child;
    uint32_t since_ms;
    char     title[96];
    char     detail[140];
} ioc_page_item;

#define IOC_PAGE_MARKS 3

typedef struct {
    uint32_t now_ms;
    int64_t  epoch_now;      /* segundos UTC em now_ms; 0 = relógio não ajustado */
    int16_t  tz_min;         /* fuso em minutos (Brasília: -180) */
    uint32_t boot_ms;
    uint8_t  battery_pct;    /* 255 = sem medida */
    uint8_t  usb_power;
    int16_t  temp_c10;
    uint16_t rh_10;
    uint32_t valid_for_s;    /* validade impressa no rodapé */
    uint8_t  serving_node;
    uint8_t  self_child;
    uint32_t normal_since_ms;          /* 0 = desde o último acontecimento observado */
    const char *marks[IOC_PAGE_MARKS];  /* marcas recentes, já formatadas, da mais nova à mais antiga */
} ioc_page_ctx;

void    ioc_page_time(const ioc_page_ctx *ctx, uint32_t at_ms, char out[8]);
int     ioc_page_assess(const ioc_view *v, const ioc_page_ctx *ctx, ioc_page_item *items, int max);
uint8_t ioc_page_level(const ioc_page_item *items, int n);
const char *ioc_page_level_title(uint8_t level);

/* Cabeçalho (faixa colorida) e rodapé (volátil); devolvem o y livre / o y do rodapé. */
int  ioc_page_header(ioc_dl *dl, uint8_t level, const char *title, const char *right);
int  ioc_page_footer(ioc_dl *dl, const ioc_page_ctx *ctx);

/* Marca IoT-over-CAN (barramento com terminações, três Nodes e o enlace sem
 * fio) centrada em [x, x + w); devolve a altura usada. */
int  ioc_page_logo(ioc_dl *dl, int x, int y, int w);

void ioc_page_situation(ioc_dl *dl, const ioc_view *v, const ioc_page_ctx *ctx,
                        const ioc_page_item *items, int n);
void ioc_page_network(ioc_dl *dl, const ioc_view *v, const ioc_page_ctx *ctx,
                      const ioc_page_item *items, int n);

/* ------------------------------------------------- política de atualização */

#define IOC_REFRESH_MIN_MS      120000u  /* entre redesenhos do mesmo nível */
#define IOC_REFRESH_DEESC_MS    180000u  /* melhora mantida antes de redesenhar */
#define IOC_REFRESH_RENEW_MS    300000u  /* renova a validade impressa com esta folga */

typedef struct {
    uint8_t  have;
    uint8_t  level;
    uint32_t hash;
    uint32_t last_ms;
    uint32_t valid_until_ms;
    uint32_t pending_since_ms;
    uint32_t pending_hash;
    uint16_t count;
} ioc_refresh;

/* 1 = redesenhar agora. *next_ms recebe quando reavaliar. Acompanha a espera
 * de uma melhora (o conteúdo precisa ficar estável por IOC_REFRESH_DEESC_MS). */
int  ioc_refresh_decide(ioc_refresh *r, uint32_t now_ms, uint32_t hash, uint8_t level, int forced,
                        uint32_t *next_ms);
void ioc_refresh_done(ioc_refresh *r, uint32_t now_ms, uint32_t hash, uint8_t level, uint32_t valid_until_ms);

#ifdef __cplusplus
}
#endif

#endif
