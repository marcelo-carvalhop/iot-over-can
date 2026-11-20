#ifndef AUTOTESTE_H
#define AUTOTESTE_H

/*
 * Comando SELFTEST: confere a placa depois de gravada, sem precisar do resto da rede.
 *
 *   SELFTEST          contadores de erro, laço interno do MCP2515 e LEDs
 *   SELFTEST CAN      só as duas primeiras partes (sem piscar os LEDs)
 *   SELFTEST LEDS     só os LEDs
 *
 * Só roda com o módulo ocioso (ainda fora da rede) ou na Probe 00: o laço interno cala o
 * controlador por alguns milissegundos e o teste dos LEDs ocupa o laço do programa por mais de
 * um segundo, o que um líder ou um seguidor em operação não pode ter. A saída é uma linha por
 * verificação e uma linha final com o resultado (ver Documentacao/guia-de-uso.md):
 *
 *   [NODE 1] [SELFTEST] CHECK=CAN_LOOPBACK RESULT=PASS REASON=OK FRAMES=4/4
 *   [NODE 1] [SELFTEST] RESULT=PASS CHECKS=2 WARN=0 FAIL=0 DURATION_MS=1043
 */
bool autotesteHandleSerialCommand(const char* command);

#endif
