# Cobertura de Requisitos

Legenda:

- **Implementado**: disponível e testado.
- **Parcial**: estrutura presente, mas depende de protocolo ou firmware.
- **Planejado**: ainda não implementado.

| Grupo | Estado | Observação |
|---|---|---|
| Conexão serial e desconexão | Implementado | seleção interativa, caminho manual e reconexão |
| Identificação gateway/sensor | Implementado | modo forçado ou autodetecção textual |
| `SENSOR_DIRECT` | Implementado | protocolo ASCII do firmware atual |
| `GATEWAY_CAN` | Parcial | protocolo textual implementado; enquadramento binário pendente |
| Modo demonstração | Implementado | quatro nós e três sensores |
| Árvore hierárquica | Implementado | gateway, nós físicos e sensores `PP.CC` |
| Telemetria resumida | Implementado | valores diretos e escalonados |
| Taxa solicitada/efetiva | Implementado | exibida quando reportada |
| Modos DSP e aquisição | Implementado | IDLE, ROTATING, STRUCTURAL, SEISMIC; POLLING nominal, SIM e DRDY legado |
| Qualidade REAL/SIM/N/A | Implementado | marcadores e estado canônico |
| Sequência de 16 bits | Implementado | perda, duplicação, fora de ordem e rollover |
| CRC16/CRC32 | Implementado | algoritmos e validação de fragmentação |
| CRC do controlador CAN | Parcial | exibido quando reportado pelo gateway |
| CAN FD 29 bits | Implementado | codificação e decodificação do ID |
| Monitor de frames | Implementado | RX/TX, ID, FD, DLC e payload |
| Ocupação do barramento | Parcial | exibe valor reportado; cálculo local pendente |
| Fragmentação | Implementado | fora de ordem, duplicados, CRC e timeout |
| FFT sob demanda | Implementado | gateway, demo e serial direta com vetor separado de `TEL` |
| FFT 32/64/128/256 bins | Implementado | modal e solicitação |
| Gráfico FFT | Implementado | eixo em Hz, magnitude, resolução, faixa e picos principais |
| Múltiplos DTCs | Implementado | por sensor |
| Limpeza de DTC | Implementado | gateway, demo e `DTC CLEAR` no sensor direto |
| Configuração remota | Implementado | direta, gateway e demo |
| `QUEUED/APPLIED/VERIFIED` | Implementado | parser e estado de transação |
| Bateria não instrumentada | Implementado | `N/A`, sem falso 100% |
| Bateria real | Parcial | exibida quando reportada |
| Logs JSONL | Implementado | gravação contínua opcional |
| Exportação CSV | Implementado | telemetria da sessão |
| Snapshot JSON | Implementado | estado completo |
| Replay | Planejado | modelos permitem adicionar `ReplayTransport` |
| SocketCAN | Planejado | transporte ainda não implementado |
| Modais | Implementado | conexão, menu, config, FFT, DTC, rede, detalhe, confirmação |
| Operação por teclado | Implementado | F1–F12 e atalhos Ctrl |
| Modo compacto | Implementado | automático e manual |
| Modo monocromático | Planejado | marcadores textuais já reduzem dependência de cor |
| Compatibilidade Textual | Implementado | validado em 1.0 e 8.2.8 |
| Teste por pseudo-terminal | Implementado | firmware simulado e fallback POSIX |
| Protocolo binário gateway | Planejado | substituirá o decoder textual sem alterar a UI |


## Atualização 0.6

| Requisito | Estado | Observação |
|---|---|---|
| `DTC CLEAR` direto | Implementado | Modal e CLI usam o comando literal |
| `ACQ POLLING` | Implementado | `Ctrl+O` e `:acq polling` |
| `POLLING` saudável | Implementado | DRDY zerado não gera falha |
| STATUS final | Implementado | Todos os campos solicitados |
| TEL final | Implementado | Inclui `WIN`, `AXIS`, `FFT_VALID`, bateria e DTC count |
| FFT inválida no SEISMIC | Implementado | Métricas ocultadas sem erro |
| FFT direta sob demanda | Implementado | `FFT ONCE` + `TELEMETRY ONCE` |
| Configuração autoritativa | Implementado | Apenas `CONFIG_APPLIED` altera estado aplicado |
| Bateria sentinela | Implementado | 255/65535 → N/A |
| Catálogo DTC mínimo | Implementado | 0x0000, 0x1002, 0x2002 e 0x4002 |
| Parada por 0x03 | Implementado | `Ctrl+C` |

## Atualização 0.7

| Requisito | Estado | Observação |
|---|---|---|
| Tela F6 dedicada | Implementado | Janela atualizada continuamente para o sensor selecionado |
| Controles na F6 | Implementado | ON, OFF, ONCE, FAST, SLOW e PERIOD |
| Pausa somente visual | Implementado | Espaço congela a janela sem interromper recepção/log |
| Tendências temporais | Implementado | Sparklines de RMS e PPV |
| FFT com eixo em Hz | Implementado | Usa `frequency = bin × Fs / N` |
| Resolução e Nyquist | Implementado | Exibe `Δf`, faixa dos bins e Nyquist |
| Picos do espectro | Implementado | Cinco maiores bins com frequência e magnitude |
| Espera dinâmica da FFT | Implementado | Modal aguarda o vetor separado após `FFT ONCE` |


- **F3 — seleção navegável de sensores:** implementado.
- **Aplicação rápida sem CLI:** implementada por botão, com confirmação por `CONFIG_APPLIED`.


## Documentação técnica de telemetria

- Guia de interpretação das grandezas: implementado no README.
- Limites universais automáticos: não implementados; dependem de baseline e validação por aplicação.
