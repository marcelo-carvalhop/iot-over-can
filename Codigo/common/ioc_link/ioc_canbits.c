#include "ioc_canbits.h"

#define FIXED_TAIL_BITS 13u /* delimitador CRC, ACK(2), EOF(7), intervalo(3) */

/* Percorre os bits SOF..dados, do primeiro ao último transmitido. */
typedef void (*bit_sink)(void *ctx, uint8_t bit);

static void emit_bits(uint32_t value, uint8_t count, bit_sink sink, void *ctx) {
    for (int i = (int)count - 1; i >= 0; --i) sink(ctx, (uint8_t)((value >> i) & 1u));
}

static void emit_header_and_data(uint16_t id11, uint8_t dlc, const uint8_t *data,
                                 bit_sink sink, void *ctx) {
    uint8_t n = dlc > 8 ? 8 : dlc;
    sink(ctx, 0);                         /* SOF dominante */
    emit_bits(id11 & 0x7FFu, 11, sink, ctx);
    sink(ctx, 0);                         /* RTR: quadro de dados */
    sink(ctx, 0);                         /* IDE: identificador de 11 bits */
    sink(ctx, 0);                         /* r0 */
    emit_bits(dlc & 0x0Fu, 4, sink, ctx);
    for (uint8_t i = 0; i < n; ++i) emit_bits(data ? data[i] : 0u, 8, sink, ctx);
}

static void crc_sink(void *ctx, uint8_t bit) {
    uint16_t *crc = (uint16_t *)ctx;
    uint8_t feedback = (uint8_t)(bit ^ ((*crc >> 14) & 1u));
    *crc = (uint16_t)((*crc << 1) & 0x7FFFu);
    if (feedback) *crc ^= 0x4599u;
}

uint16_t ioc_can_crc15(uint16_t id11, uint8_t dlc, const uint8_t *data) {
    uint16_t crc = 0;
    emit_header_and_data(id11, dlc, data, crc_sink, &crc);
    return crc;
}

typedef struct {
    uint8_t last;   /* valor do último bit no barramento */
    uint8_t run;    /* quantos bits iguais consecutivos */
    uint8_t stuff;  /* bits de preenchimento inseridos */
    uint8_t started;
} stuff_state;

static void stuff_sink(void *ctx, uint8_t bit) {
    stuff_state *s = (stuff_state *)ctx;
    if (!s->started) {
        s->started = 1;
        s->last = bit;
        s->run = 1;
        return;
    }
    if (bit == s->last) {
        s->run++;
    } else {
        s->last = bit;
        s->run = 1;
    }
    if (s->run == 5) {
        /* O transmissor insere o complemento, que inicia uma nova sequência. */
        s->stuff++;
        s->last = (uint8_t)!s->last;
        s->run = 1;
    }
}

uint8_t ioc_can_stuff_bits(uint16_t id11, uint8_t dlc, const uint8_t *data) {
    stuff_state state = {0, 0, 0, 0};
    uint16_t crc = ioc_can_crc15(id11, dlc, data);
    emit_header_and_data(id11, dlc, data, stuff_sink, &state);
    emit_bits(crc, 15, stuff_sink, &state);
    return state.stuff;
}

uint16_t ioc_can_frame_bits(uint16_t id11, uint8_t dlc, const uint8_t *data) {
    uint8_t n = dlc > 8 ? 8 : dlc;
    return (uint16_t)(34u + 8u * n + ioc_can_stuff_bits(id11, dlc, data) + FIXED_TAIL_BITS);
}

uint16_t ioc_can_frame_bits_worst(uint8_t dlc) {
    uint8_t n = dlc > 8 ? 8 : dlc;
    uint16_t stuffable = (uint16_t)(34u + 8u * n);
    return (uint16_t)(stuffable + (stuffable - 1u) / 4u + FIXED_TAIL_BITS);
}
