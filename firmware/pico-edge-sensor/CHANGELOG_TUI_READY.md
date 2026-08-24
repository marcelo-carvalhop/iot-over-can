# CHANGELOG - TUI Ready Robustness Pass

## Implementado

- Bateria por MAX17048/MAX17043 em I2C1.
- Beacon com bateria real, tensão, modo de aquisição, contagem de DTC e versão de protocolo.
- DTC de baixa tensão operacional.
- Tabela de DTCs ativos com supressão de duplicados.
- `DTC CLEAR` serial e `CMD_CLEAR_DTC` UDP.
- DTC events assíncronos também para warnings.
- Freeze frame com temperatura MPU, temperatura RP e tensão de bateria.
- DSP tri-axial por magnitude vetorial.
- Janela configurável 128/256/512.
- IDLE pausa aquisição.
- Telemetria UDP expandida para refletir a serial.
- FFT marcada como válida/inválida.
- ACK de configuração em duas fases: queued e applied.
- Modo de aquisição estruturado em STATUS e telemetria.
- Recomendações de integração CAN/gateway adicionadas.

## Observação

O nome `mpu6050_dma_driver.*` foi mantido por compatibilidade, mas o driver segue a arquitetura DRDY sem DMA.
