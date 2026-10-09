/**
 * @file ioc_canbits.h
 * @brief Duração exata de um quadro CAN clássico no barramento, em bits.
 *
 * A Probe 00 usa estas funções para medir a ocupação do barramento sem
 * hardware adicional: o MCP2515 entrega identificador, DLC e dados, e o
 * restante do quadro (CRC-15 e bits de preenchimento) é reconstruído aqui.
 *
 * Quadro de dados com identificador de 11 bits:
 *
 *   SOF(1) ID(11) RTR(1) IDE(1) r0(1) DLC(4) dados(8n) CRC(15)   <- sujeito a bit stuffing
 *   delimitador CRC(1) ACK(2) EOF(7) intervalo entre quadros(3)  <- 13 bits fixos
 *
 * Total: 47 + 8n bits, mais os bits de preenchimento.
 */
#ifndef IOC_CANBITS_H
#define IOC_CANBITS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** CRC-15 do CAN (polinômio 0x4599) sobre SOF..dados de um quadro de 11 bits. */
uint16_t ioc_can_crc15(uint16_t id11, uint8_t dlc, const uint8_t *data);

/** Bits de preenchimento realmente inseridos no quadro. */
uint8_t ioc_can_stuff_bits(uint16_t id11, uint8_t dlc, const uint8_t *data);

/** Ocupação do barramento pelo quadro, incluindo o intervalo entre quadros. */
uint16_t ioc_can_frame_bits(uint16_t id11, uint8_t dlc, const uint8_t *data);

/** Limite superior para um quadro com `dlc` bytes (pior caso de preenchimento). */
uint16_t ioc_can_frame_bits_worst(uint8_t dlc);

#ifdef __cplusplus
}
#endif

#endif /* IOC_CANBITS_H */
