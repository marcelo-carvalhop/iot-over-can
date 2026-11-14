#ifndef ENERGIA_H
#define ENERGIA_H

#include <Arduino.h>

/*
 * Alimentação do Módulo CAN: motivo dos reinícios e limite do rádio.
 *
 * Com BLE e o ponto de acesso Wi-Fi ligados, o ESP32 puxa rajadas de 300 a
 * 500 mA. Numa bancada com protoboard e cabos longos isso derruba a tensão e
 * faz o GND "pular": os LEDs piscam e, no pior caso, a placa reinicia (e acende
 * os três LEDs de uma vez, como na partida). Este módulo ajuda a distinguir os
 * casos e reduz o pico:
 *
 *  - motivo de cada partida e contagem dos reinícios anormais (queda de tensão,
 *    watchdog, falha do programa), gravada na NVS: só esses reinícios gravam,
 *    ligar a placa ou abrir a porta serial não desgasta a flash;
 *  - comando serial ENERGIA (mostra o estado) e ENERGIA LIMPAR (zera a contagem);
 *  - potência máxima do Wi-Fi (IOT_WIFI_TX_POWER_QDBM).
 *
 * A lógica de classificação e o formato gravado estão em ioc_resetlog.h, testada
 * no computador.
 */

/*
 * Potência máxima de transmissão do ponto de acesso, em quartos de dBm, como em
 * esp_wifi_set_max_tx_power() (8 a 84, de 2 a 20 dBm; só os degraus 8, 20, 28,
 * 34, 44, 52, 56, 60, 66, 72 e 80 fazem diferença). O padrão do ESP32 é 78
 * (19,5 dBm), com pico de 240 mA no datasheet. Aqui o padrão é 44 (11 dBm): na
 * bancada, com o sensor a poucos metros, sobra margem de sinal e o pico de
 * corrente da transmissão diminui. Para alcance maior, compile com
 * -D IOT_WIFI_TX_POWER_QDBM=78.
 */
#ifndef IOT_WIFI_TX_POWER_QDBM
#define IOT_WIFI_TX_POWER_QDBM 44
#endif
static_assert(IOT_WIFI_TX_POWER_QDBM >= 8 && IOT_WIFI_TX_POWER_QDBM <= 84,
              "IOT_WIFI_TX_POWER_QDBM deve estar entre 8 e 84 (quartos de dBm)");

/* Na partida: registra o motivo; avisa, e grava, só se o reinício foi anormal. */
void energiaInit();

/* ENERGIA e ENERGIA LIMPAR. Retorna true se o comando era um desses. */
bool energiaHandleSerialCommand(const char* command);

#endif
