# Notas de Integração

## Objetivo

Esta revisão combina duas bases:

1. o programa `pico_tui`, escolhido como referência visual e de interação;
2. o núcleo da versão anterior da TUI, que já possuía modelos de domínio, parsers e serviços voltados ao gateway e à rede CAN FD.

A integração não foi realizada por simples cópia de telas. O padrão visual foi preservado, mas o estado interno foi substituído por uma arquitetura hierárquica e orientada a eventos.

## Elementos preservados do `pico_tui`

- paleta quente outonal;
- disposição em painéis;
- tabela de telemetria;
- sparkline de RMS;
- configuração por listas;
- linha de comando;
- tela de conexão;
- navegação baseada em teclado;
- console direto compatível com o firmware do Pico.

## Elementos incorporados do núcleo anterior

- `StateStore` central;
- eventos tipados;
- `EventBus`;
- identificação `parent_node_id + child_id + UUID`;
- árvore de gateway, nós e sensores;
- gateway CAN FD;
- ID estendido de 29 bits;
- contador de sequência de 16 bits;
- CRC16/CRC32;
- remontagem de fragmentos;
- múltiplos DTCs;
- logs JSONL;
- CSV e snapshot;
- mensagens legadas da fase 1;
- rede simulada.

## Correções realizadas na base visual

### Transporte por pseudo-terminal

O pySerial pode falhar ao abrir pseudoportas POSIX em alguns ambientes por tentar executar operações de `termios` não aceitas pelo descritor. Foi incluído um backend POSIX mínimo como fallback. Ele é usado apenas quando a abertura convencional falha por essa condição.

### Eco e prompt

O firmware direto ecoa os caracteres recebidos e produz `EDGE>` sem necessariamente terminar a linha. O filtro foi mantido e reforçado para não confundir eco, prompt e eventos assíncronos.

### Estado único

Variáveis isoladas como `fsm_mode`, `simulate_on` e `telemetry_on` continuam expostas apenas para compatibilidade, mas são sincronizadas a partir do estado canônico. Elas não são mais a fonte primária da interface.

### Compatibilidade entre versões do Textual

No Textual 1.0, `App.query_one` pode pesquisar apenas a tela modal atualmente ativa. A atualização periódica agora tolera a ausência temporária dos widgets da tela principal enquanto um modal estiver aberto. O mesmo código foi validado no Textual 1.0 e no 8.2.8.

### Timeout de fragmentação

Transferências incompletas agora expiram periodicamente mesmo quando nenhum fragmento novo chega. Antes, a expiração dependia da chegada de uma nova mensagem ao remontador.

## Decisões arquiteturais

- A interface é desacoplada do transporte.
- `SENSOR_DIRECT` e `GATEWAY_CAN` alimentam os mesmos modelos.
- A árvore hierárquica substitui a suposição de sensor único.
- A telemetria do CAN utiliza grandezas já normalizadas no modelo, mas preserva o payload bruto.
- FFT permanece sob demanda.
- Frames desconhecidos são preservados no monitor bruto.
- A aplicação inicia em modo de leitura e só envia comandos após ação explícita do operador.

## Pontos que dependem do firmware

A TUI está preparada, mas ainda depende de decisões externas para:

- enquadramento binário do gateway;
- tabela final de domínios;
- tabela final de tipos de mensagem;
- formato exato de ACK e transações;
- suporte a limpeza de DTC no Pico;
- FFT pela serial direta;
- cálculo ou reporte da ocupação CAN FD;
- timestamps sincronizados.


## Baseline 0.6 — aquisição POLLING

A aquisição por polling deixou de ser fallback e passou a ser o modo nominal. O estado DRDY foi mantido somente para leitura de logs antigos. A TUI não usa `DRDY_IRQ` nem `DRDY_MISSED` como indicadores de falha enquanto `ACQ=POLLING`.

A configuração principal é atualizada somente pelo evento `CONFIG_APPLIED`. ACKs `STAGED` e `APPLY_QUEUED` atualizam apenas o estado da transação.

## Revisão 0.7

- F6 deixou de apenas mover o foco para a tabela principal e passou a abrir `TelemetryScreen`.
- A tela recebe snapshots do `StateStore`; não acessa a serial diretamente.
- Comandos da tela são publicados como `TelemetryCommandRequested` e executados pelo `PicoTuiApp`.
- `FftViewScreen` passou a consultar o estado periodicamente e aguardar uma FFT posterior ao instante da solicitação.
- A escala em frequência é calculada somente quando `sample_rate_hz` e `fft_size` estão disponíveis.
