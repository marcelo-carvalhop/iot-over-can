# Correção: DRDY/INT sem DMA

Esta versão remove o DMA do driver do MPU6050 e mantém o pino INT/DRDY como referência de amostragem.

## O que mudou

- `hardware_dma` foi removido do `CMakeLists.txt`.
- `mpu6050_dma_driver.c` foi reescrito sem `hardware/dma.h`.
- O nome do arquivo foi mantido para não quebrar os includes e o CMake.
- A ISR do GP2/DRDY não faz leitura I²C.
- A ISR apenas incrementa um contador de amostras pendentes.
- A leitura I²C burst de 6 bytes ocorre fora da ISR, no contexto do loop principal.
- O ping-pong buffer continua existindo.
- Quando 512 amostras são preenchidas, o buffer é entregue ao DSP.
- O timeout de fallback agora considera a taxa de amostragem:
  - 100 Hz -> buffer esperado em ~5,12 s
  - 250 Hz -> buffer esperado em ~2,05 s
  - 500 Hz -> buffer esperado em ~1,02 s
  - 1000 Hz -> buffer esperado em ~0,512 s

## Motivo

O caminho anterior tentava realizar operação I²C dentro da ISR de GPIO e depois acionar DMA. Isso era frágil para bancada e dificultava depuração. O novo desenho preserva sincronização por hardware, mas deixa a transação I²C fora da interrupção.
