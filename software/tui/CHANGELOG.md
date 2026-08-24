# Changelog

## 0.8.1

- Removido o botão redundante **Abrir F4** da Configuração Rápida; permanece somente **Aplicar**.
- Adicionado ao README um guia de grandezas com definição, interpretação no projeto e referências iniciais.
- Documentadas as limitações metrológicas do PPV calculado por integração da aceleração.
- Adicionadas referências técnicas para vibração de máquinas, PPV, STA/LTA e MPU-6050.

## 0.8.0

- F3 agora abre uma janela hierárquica para navegar e selecionar sensores com ↑/↓ e Enter.
- O sensor selecionado recebe um marcador visual na árvore principal.
- A coluna Configuração Rápida agora possui botão Aplicar.
- O botão Aplicar envia APPLY no modo direto e aguarda CONFIG_APPLIED antes de considerar os valores efetivos.
- Adicionado botão Abrir F4 na configuração rápida.
- Novos testes de navegação F3 e aplicação rápida.

## 0.7.0

- `F6` agora abre uma tela dedicada de telemetria em tempo real.
- Adicionados controles de streaming na tela F6: ON, OFF, ONCE, FAST, SLOW e PERIOD.
- Abrir ou fechar F6 não altera implicitamente o estado do firmware.
- Adicionadas tendências independentes de RMS e PPV.
- `Espaço` pausa somente a atualização visual da tela detalhada.
- `F7` agora abre uma visualização dinâmica após solicitar a FFT.
- A janela FFT aguarda a resposta separada de `FFT ONCE` em vez de esperar o vetor dentro de `TEL`.
- Adicionados eixo de frequência, escala de magnitude, `Δf`, Nyquist, faixa coberta e picos principais.
- A unidade vertical permanece explícita como `raw`, `u16`, `float` ou outra reportada pelo protocolo.
- Adicionados testes de F6, comandos da janela e fluxo completo de FFT direta.

## 0.6.0

- Corrigido `DTC CLEAR` no modo `SENSOR_DIRECT`, tanto pelo modal quanto pelo prompt interno.
- Adicionado `ACQ POLLING` e atalho global `Ctrl+O`.
- `POLLING` passou a ser o modo oficial e saudável de aquisição.
- `ACQ DRDY` passou a ser tratado como comando legado rejeitado, sem degradar o sensor.
- Atualizado o enum de aquisição para `UNKNOWN`, `POLLING`, `SIM`, `IDLE` e `DRDY` legado.
- Atualizados os parsers de `STATUS` e `TEL` para os formatos finais.
- Implementado `FFT_VALID`; métricas espectrais são ocultadas quando inválidas.
- Implementada FFT direta sob demanda por `FFT ONCE` seguido de `TELEMETRY ONCE`.
- Corrigido o fluxo de configuração: `OK STAGED` e `OK APPLY_QUEUED` não alteram o estado aplicado.
- Adicionado suporte a `CONFIG_APPLIED` como confirmação autoritativa.
- `BATT_PCT=255` e `BATT_MV=65535` passam a ser exibidos como `N/A`.
- Adicionado catálogo inicial de DTCs e indicação explícita de clipping.
- Adicionado `Ctrl+C` para interromper telemetria enviando o byte `0x03`.
- Atualizados firmware simulado, documentação e testes.

## 0.5.0

- Integração do padrão visual do `pico_tui` com a arquitetura hierárquica CAN FD.
