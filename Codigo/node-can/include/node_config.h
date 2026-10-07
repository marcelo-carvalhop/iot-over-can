#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include <stdint.h>

/*
 * Identificador logico do no.
 *
 * Deve ser definido no arquivo principal .ino.
 *
 * Regras:
 *   NODE_ID = 0  -> Probe 00
 *   NODE_ID > 0  -> No funcional
 *
 * Cada no da rede deve possuir NODE_ID unico.
 */
extern const uint8_t NODE_ID;

#endif
