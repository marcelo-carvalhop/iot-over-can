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
 *  - potência máxima do Wi-Fi (IOT_WIFI_TX_POWER_QDBM);
 *  - medição, desligada por padrão, da tensão de entrada (divisor resistivo num pino do ADC1) e da corrente
 *    (INA219 no I2C), que aparece na linha do comando ENERGIA. Só mede e informa (ioc_power.h).
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

/*
 * Medição da alimentação. Cada opção só existe se for definida na compilação; sem -D nada disto é compilado e
 * a linha do ENERGIA continua como era.
 *
 *   -D IOT_VIN_ADC_PIN=34      pino do ADC1 (GPIO 32 a 39) ligado ao meio do divisor: Vin ─ R1 ─┬─ R2 ─ GND.
 *                              O ADC2 não serve: o Wi-Fi o usa.
 *   -D IOT_VIN_R1_OHM=100000   R1 do divisor (padrão 100 kΩ)
 *   -D IOT_VIN_R2_OHM=100000   R2 do divisor (padrão 100 kΩ; com 5 V na entrada o pino recebe 2,5 V)
 *   -D IOT_VIN_WARN_MV=4750    abaixo disto a tensão é BAIXA e o módulo avisa (no máximo a cada 10 s)
 *   -D IOT_VIN_CRIT_MV=4500    abaixo disto é CRITICA
 *
 *   -D IOT_INA219_ADDR=64      endereço I2C do INA219 (0x40 a 0x4F, em decimal 64 a 79); liga a medição de corrente
 *   -D IOT_INA219_SHUNT_MOHM=100  resistor de derivação em mΩ (as placas comuns trazem 0,1 Ω)
 *   -D IOT_I2C_SDA=21 -D IOT_I2C_SCL=22   pinos do I2C
 *
 * Falta de tensão de entrada não é o mesmo que queda de tensão no 3,3 V: o ESP32 reinicia quando o 3,3 V cai
 * abaixo de cerca de 2,5 V, e a tensão de entrada mostra quanto sobrava antes disso.
 */
#ifndef IOT_VIN_ADC_PIN
#define IOT_VIN_ADC_PIN (-1)
#endif
#ifndef IOT_VIN_R1_OHM
#define IOT_VIN_R1_OHM 100000
#endif
#ifndef IOT_VIN_R2_OHM
#define IOT_VIN_R2_OHM 100000
#endif
#ifndef IOT_VIN_WARN_MV
#define IOT_VIN_WARN_MV 4750
#endif
#ifndef IOT_VIN_CRIT_MV
#define IOT_VIN_CRIT_MV 4500
#endif
#ifndef IOT_INA219_ADDR
#define IOT_INA219_ADDR (-1)
#endif
#ifndef IOT_INA219_SHUNT_MOHM
#define IOT_INA219_SHUNT_MOHM 100
#endif
#ifndef IOT_I2C_SDA
#define IOT_I2C_SDA 21
#endif
#ifndef IOT_I2C_SCL
#define IOT_I2C_SCL 22
#endif
static_assert(IOT_VIN_ADC_PIN < 0 || (IOT_VIN_ADC_PIN >= 32 && IOT_VIN_ADC_PIN <= 39),
              "IOT_VIN_ADC_PIN deve ser um pino do ADC1 do ESP32 (GPIO 32 a 39); o ADC2 e o Wi-Fi se excluem");
static_assert(IOT_VIN_R2_OHM > 0 && IOT_VIN_R1_OHM > 0, "o divisor precisa de R1 e R2 maiores que zero");
static_assert(IOT_VIN_CRIT_MV <= IOT_VIN_WARN_MV, "IOT_VIN_CRIT_MV não pode passar de IOT_VIN_WARN_MV");
static_assert(IOT_INA219_ADDR < 0 || (IOT_INA219_ADDR >= 0x40 && IOT_INA219_ADDR <= 0x4F),
              "IOT_INA219_ADDR deve ficar entre 0x40 e 0x4F (64 a 79)");
static_assert(IOT_INA219_SHUNT_MOHM > 0, "IOT_INA219_SHUNT_MOHM deve ser maior que zero");

/* Na partida: registra o motivo; avisa, e grava, só se o reinício foi anormal. */
void energiaInit();

/* A cada passagem do laço: colhe uma amostra da tensão e da corrente a cada 10 ms, se ligadas. Sem custo se não. */
void energiaPoll();

/* ENERGIA e ENERGIA LIMPAR. Retorna true se o comando era um desses. */
bool energiaHandleSerialCommand(const char* command);

#endif
