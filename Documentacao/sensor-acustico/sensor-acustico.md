# Sensor acústico INMP441

O segundo sensor sem fio do projeto mede som. Um microfone MEMS digital INMP441, ligado por I2S a um Raspberry Pi Pico W (RP2040) ou Pico 2 W (RP2350), alimenta um motor de análise que roda inteiro no segundo núcleo do microcontrolador. A cada segundo o sensor publica um relatório com níveis sonoros já integrados e os resultados do modo em uso; o restante do sistema — oferta BLE, enlace autenticado, Node CAN, Probe 00 e TUI — é o mesmo do sensor de vibração.

Os trechos marcados com `livro:inicio`/`livro:fim` compõem o capítulo correspondente do livro do projeto; ver [`../livro.md`](../livro.md).

<!-- livro:inicio id="acustico-visao-geral" capitulo="Sensor acústico INMP441" -->
## 1. Visão geral

O sensor de vibração responde três perguntas sobre uma máquina ou estrutura — como ela gira (modo `ROTATING`), quanto ela vibra (`STRUCTURAL`) e quando algo a sacode (`SEISMIC`). O sensor acústico faz as mesmas três perguntas pelo som:

| Modo acústico | Pergunta | Análogo no sensor de vibração | Resultado principal |
|---|---|---|---|
| `LEVEL` | Quanto ruído há aqui, e quanto dele uma pessoa recebe? | `STRUCTURAL` (nível de vibração) | LAeq,T, L10/L50/L90, doses NR-15 e NHO-01 |
| `MACHINE` | Como esta máquina soa, e soa como de costume? | `ROTATING` (máquina rotativa) | fundamental e harmônicos ("tacômetro acústico"), proeminência tonal, escore de anomalia |
| `EVENTS` | Aconteceu algum som fora do comum? | `SEISMIC` (gatilho STA/LTA) | eventos detectados, classe, pico, SEL e duração |
| `IDLE` | — | `IDLE` | microfone sem relógio, nada é medido |

Os números dos modos repetem os do sensor de vibração (1, 2 e 3); o nome só tem significado junto com o perfil do sensor (`0x02`, `PROFILE_ACOUSTIC_MEMS`), que viaja no HELLO autenticado. Por isso o Node recusa um modo de um perfil enviado ao sensor do outro (ver *Comandos pela rede*).

Caminho de um relatório, do ar à tela:

```text
INMP441 ──I2S──► PIO ──DMA (anel de 16 blocos)──► núcleo 1: DSP ──fila──► núcleo 0
                                                                            │
   TUI ◄── Probe 00 (TEL PROFILE=ACOUSTIC) ◄── CAN 0x380+N (19 quadros) ◄── Node CAN ◄── Wi-Fi/UDP (DATA_UP)
```
<!-- livro:fim id="acustico-visao-geral" -->

<!-- livro:inicio id="acustico-hardware" capitulo="Sensor acústico INMP441" -->
## 2. Hardware e ligação

O INMP441 entrega amostras de 24 bits em complemento de dois por I2S. As características que importam para a medição:

| Grandeza | Valor (folha de dados) | Consequência no projeto |
|---|---|---|
| Sensibilidade | −26 dBFS a 94 dB SPL, 1 kHz (±1 dB) | define a escala: 1 unidade normalizada = 28,2843 Pa (ver *Da palavra I2S à pressão*) |
| Ponto de sobrecarga acústica | 120 dB SPL | pico máximo mensurável; acima disso a amostra satura |
| Relação sinal/ruído | 61 dBA | ruído próprio perto de 33 dB(A): ambientes muito silenciosos ficam no piso |
| Resposta em frequência | 60 Hz a 15 kHz | bandas de 31,5 Hz e de 16 kHz são indicativas |
| Frequência de WS | 7,8 a 50 kHz | taxas aceitas pelo firmware: 8, 16, 24, 32 e 48 kHz |
| Partida | 2¹⁸ ciclos de SCK com saída em zero | 256 ms a 16 kHz, 85 ms a 48 kHz descartados após cada (re)início |

Ligação padrão, igual nas duas placas (pinos físicos vizinhos no conector):

| INMP441 | Pico W / Pico 2 W | Pino físico | Observação |
|---|---|---|---|
| SCK | GP18 | 24 | relógio de bit, 64 × fs |
| WS | GP19 | 25 | sempre SCK + 1: exigência do *side-set* da PIO |
| SD | GP20 | 26 | dado serial |
| L/R | GND | 23 | canal esquerdo; em VDD o microfone responde no direito |
| VDD | 3V3(OUT) | 36 | |
| GND | GND | 23 ou 28 | |

Os pinos podem ser trocados no build (`-DAUDIO_PIN_SCK=..`, `-DAUDIO_PIN_SD=..`). O pino L/R não precisa ser conhecido de antemão: se o canal em uso ficar em silêncio digital por 1 s, o firmware tenta o outro; depois de uma tentativa em cada lado, permanece no esquerdo e passa a acusar a falha (DTC `0x1003`). O console mostra cada troca em uma linha `[MIC]`.

### Pico W ou Pico 2 W

O mesmo código compila para as duas placas (`PICO_BOARD=pico_w` ou `pico2_w`); só a configuração inicial muda:

| Placa | Núcleo | Configuração inicial | Motivo |
|---|---|---|---|
| Pico W (RP2040, 125 MHz) | Cortex-M0+ sem unidade de ponto flutuante | 16 kHz, FFT de 1024 pontos | o ponto flutuante é emulado; 16 kHz cobre até 8 kHz com folga de processamento |
| Pico 2 W (RP2350, 150 MHz) | Cortex-M33 com FPU | 48 kHz, FFT de 2048 pontos | acompanha a banda inteira do INMP441 |

A ocupação do núcleo de DSP aparece em cada relatório (`CPU_PCT`) e a fração do áudio que passou pela FFT em `COVERAGE_PCT`; os dois números devem ser conferidos em bancada antes de usar 32 ou 48 kHz no RP2040.
<!-- livro:fim id="acustico-hardware" -->

<!-- livro:inicio id="acustico-aquisicao" capitulo="Sensor acústico INMP441" -->
## 3. Aquisição: I2S na PIO e DMA em anel

O RP2040 e o RP2350 não têm periférico I2S; o barramento é gerado por uma máquina de estados da PIO (`inmp441_i2s.pio`). O programa tem 12 instruções e usa o *side-set* de dois bits para gerar SCK (bit 0) e WS (bit 1): cada período de SCK ocupa duas instruções, de modo que a PIO roda a 128 × fs. O dado é lido apenas nas bordas de subida do SCK, quando o INMP441 o mantém estável, e os 32 bits do canal esquerdo são empacotados por *autopush* em uma palavra por quadro estéreo. O canal direito é lido com o mesmo programa: o firmware inverte WS no próprio pino (override de saída do GPIO), e a PIO continua "lendo o esquerdo".

O divisor da PIO tem 16 bits inteiros e 8 fracionários; a taxa efetiva é recalculada a partir do divisor arredondado e é a que entra em todos os cálculos:

| Pedido | RP2040 a 125 MHz | RP2350 a 150 MHz |
|---|---|---|
| 8 e 16 kHz | exatos | exatos |
| 24 kHz | 23 999,23 Hz | exatos |
| 32 kHz | 32 002,05 Hz | exatos |
| 48 kHz | 48 003,07 Hz | exatos |

Dois canais de DMA encadeados (A → B → A) copiam o FIFO da PIO para um anel de 16 blocos de 256 palavras (256 ms de áudio a 16 kHz, 85 ms a 48 kHz). Quando um canal termina, o outro já escreve o bloco seguinte; a interrupção (`DMA_IRQ_1`, compartilhada) apenas conta o bloco e reaponta o canal que terminou. Nenhuma amostra depende da latência do processamento. Se o DSP ficar mais de 14 blocos para trás, os mais antigos são descartados, contados como perdidos e informados ao motor, que mantém a dose com o último nível lento conhecido (ver *LEVEL — nível sonoro e exposição*) e acusa o DTC `0x3001`.

O rádio CYW43 também usa uma máquina de estados e canais de DMA. Por isso o microfone reserva os seus dinamicamente (`pio_claim_free_sm_and_add_program_for_gpio_range`, `dma_claim_unused_channel`), depois de o rádio ter reservado os dele. A parada respeita a errata RP2040-E13: o encadeamento é desfeito antes do `abort`.
<!-- livro:fim id="acustico-aquisicao" -->

<!-- livro:inicio id="acustico-firmware" capitulo="Sensor acústico INMP441" -->
## 4. Firmware em dois núcleos

| Núcleo | Tarefa | Arquivos |
|---|---|---|
| 0 | Wi-Fi, BLE, enlace autenticado, console USB, diagnóstico (DTC), vigilância | `main.c`, `audio_app.c`, `audio_console.c` e os módulos comuns de `Codigo/node-wifi` |
| 1 | aquisição e DSP | `audio_core1.c`, `inmp441_i2s.c`, `acoustic_dsp.c`, `audio_fft.c` |

Os núcleos conversam por três filas (`pico/util/queue.h`): comandos (configuração e controle) do 0 para o 1, resultados e relatórios do 1 para o 0. O núcleo 1 tem pilha própria de 8 KiB e um motor de cerca de 70 KiB que só ele toca. O núcleo 0 só alimenta o *watchdog* (8 s) enquanto o contador de vida do núcleo 1 avança; 2 s sem progresso geram o DTC `0x3002` e, sem recuperação, a reinicialização.

O enlace não foi copiado. Oferta BLE, HELLO/CHALLENGE/CONFIRM, sessão, contadores anti-repetição e DTCs ficam em `Codigo/node-wifi` e são compartilhados pelos dois firmwares por meio de ganchos definidos em `edge_app.h` (perfil anunciado, menu de configuração, taxa e janela efetivas, fila de configuração e comandos próprios da sessão). Cada firmware implementa os ganchos e o ligador escolhe a implementação; o firmware de vibração usa `edge_app_vibration.c`, o acústico `audio_app.c`. A validação de configuração segue o mesmo contrato (`config_validation.h`), com as faixas de cada sensor.
<!-- livro:fim id="acustico-firmware" -->

<!-- livro:inicio id="acustico-cadeia-de-medicao" capitulo="Sensor acústico INMP441" -->
## 5. Cadeia de medição

### 5.1 Da palavra I2S à pressão

A palavra de 32 bits traz o dado de 24 bits alinhado à esquerda; `s = palavra >> 8`. Pela convenção da folha de dados, 0 dBFS é a senoide de fundo de escala, cujo valor eficaz é 1/√2 do fundo de escala. Uma senoide de 94 dB SPL (1 Pa eficaz) aparece a −26 dBFS, ou seja, com valor eficaz de 10^(−26/20)/√2 = 0,03536 do fundo de escala. Daí

```text
p [Pa] = (s / 2^23) × 28,2843 × ganho
```

e a senoide de fundo de escala corresponde a 20 Pa eficazes, 120 dB SPL — exatamente o ponto de sobrecarga do microfone. O ganho de calibração (1,0 = sensibilidade nominal) corrige a tolerância de ±1 dB do microfone e é ajustado com um calibrador acústico (ver *Calibração*). O nível de offset (componente contínua) é estimado por média móvel e removido antes de qualquer energia.

### 5.2 Quatro escalas de tempo

| Escala | Duração | O que acontece |
|---|---|---|
| amostra | 1/fs | só aritmética inteira: soma, soma dos quadrados, pico, detecção de saturação e de microfone mudo; cópia para o quadro da FFT |
| subquadro | 10 ms | energia sem ponderação; ponderações temporais F (125 ms) e S (1 s); histograma do LAF; dose; detector STA/LTA |
| quadro FFT | N amostras (256 a 2048) | espectro de potência com janela, energias com ponderação A e C, média de Welch, curtose |
| relatório | 1 s | fecha os níveis do intervalo e produz o relatório |

A base é o domínio do tempo: o LZeq soma a energia de todas as amostras, sem lacunas. A FFT pode não acompanhar todos os quadros (no RP2040, a taxas altas, ou no modo EVENTS, que só a usa durante eventos), e por isso as ponderações em frequência não são aplicadas diretamente ao nível. O firmware mede, nos quadros que passam pela FFT, a razão entre a energia ponderada e a não ponderada e a aplica à energia exata do tempo:

```text
LAeq = 10·log10( E_Z(tempo) × E_A(FFT) / E_Z(FFT) / p0² )
```

A correção do janelamento é a mesma no numerador e no denominador e cancela. As curvas A e C são as expressões analíticas da IEC 61672-1 (polos em 20,6 Hz, 107,7 Hz, 737,9 Hz e 12 194 Hz), avaliadas no centro de cada faixa.

### 5.3 Métricas de todos os modos

| Métrica | Definição |
|---|---|
| LAeq, LCeq, LZeq | nível equivalente do último segundo, ponderações A, C e Z |
| LZpeak | pico da pressão sem ponderação no segundo |
| LAF, LAFmax, LAFmin | nível A com ponderação rápida (τ = 125 ms) no fim do segundo, máximo e mínimo |
| fator de crista | pico / valor eficaz, sem ponderação |
| curtose | excesso de curtose das amostras (0 para ruído gaussiano; impactos a elevam) |
| frequência dominante | maior faixa do espectro médio, refinada por interpolação parabólica em escala logarítmica |
| centroide | média das frequências ponderada pela potência |
| planura espectral | média geométrica / média aritmética da potência de 50 Hz a 90 % de Nyquist (0 para tons puros, 1 para ruído branco) |
| bandas de oitava | soma da potência das faixas em [fc/√2, fc·√2), de 31,5 Hz a 16 kHz (ISO 266); banda cujo limite superior passa de fs/2 é omitida |
| espectro enviado | até 256 faixas de 0 a fs/2, em pressão eficaz (Pa); grupos de faixas somados em potência |

A política de FFT depende do atraso do DMA: até 8 blocos de atraso todos os quadros pedidos passam pela FFT; de 9 a 12, só os quadros de evento; acima disso, nenhum. `COVERAGE_PCT` informa a fração analisada.

Um microfone desligado ou com SD aberto produz palavras todas em zero; SD preso em nível alto, todas em um. Um segundo inteiro assim marca `MIC_FAULT` e gera o DTC `0x1003`.
<!-- livro:fim id="acustico-cadeia-de-medicao" -->

<!-- livro:inicio id="acustico-modos" capitulo="Sensor acústico INMP441" -->
## 6. Modos de operação

### 6.1 LEVEL — nível sonoro e exposição

Mede o ambiente como um medidor integrador e um dosímetro, desde o último reinício da medição (`MEASURE RESET`):

- **LAeq,T**: energia A acumulada dividida pelo tempo decorrido.
- **L10, L50, L90**: níveis excedidos em 10, 50 e 90 % do tempo, a partir de um histograma do LAF com resolução de 0,5 dB (um registro a cada 10 ms).
- **Doses**: a cada subquadro de 10 ms com LAS (ponderação lenta) de 80 dB(A) ou mais, soma-se `t / T(L)`, com `T(L) = 8 h / 2^((L − 85) / q)`:
  - NR-15, Anexo 1: critério de 85 dB(A) para 8 h, incremento de duplicação q = 5 dB;
  - NHO-01 (Fundacentro): mesmo critério, q = 3 dB.
- **Limite**: a sinalização `DOSE_LIMIT` acende quando qualquer dose chega a 100 % ou quando o LAS passa de 115 dB(A), nível acima do qual a NR-15 não admite exposição sem proteção.

### 6.2 MACHINE — diagnóstico de máquina

Para máquinas, o espectro médio de Welch (todos os quadros do segundo) é examinado de três formas:

- **Proeminência tonal**: potência do componente dominante (3 faixas) contra a densidade mediana do ruído no intervalo de ±1/6 de oitava em volta, sem as faixas do tom. A mediana ignora harmônicos vizinhos que caiam na referência.
- **Tacômetro acústico**: a fundamental de uma série harmônica é procurada pelo produto harmônico do espectro (HPS, cinco harmônicos, soma dos logaritmos) numa grade de 1/4 de faixa. O candidato só é aceito com pelo menos três harmônicos 6 dB acima dos vales entre eles, incluindo o 1º ou o 2º; a frequência é então refinada pelo harmônico mais forte. A busca vai de max(4·Δf, 20 Hz) a (N/2 − 2)·Δf/5, com Δf = fs/N: a 16 kHz com 1024 pontos, de 62,5 a 1594 Hz; para um motor de 1800 rpm (30 Hz) use 8 kHz com 2048 pontos (de 20 a 798 Hz).
- **Assinatura e anomalia**: `BASELINE LEARN <s>` registra, por banda de oitava, média e variância (algoritmo de Welford) durante 5 a 3600 s de operação normal. Depois disso, cada segundo recebe um escore

  ```text
  z = √( média_b [ (L_b − média_b) / max(desvio_b, 0,5 dB) ]² )
  ```

  e `ANOMALY` acende quando z passa do limiar configurado (3 é um bom começo; o console e a TUI o propõem ao escolher o modo). A assinatura sobrevive a reconfigurações que mantêm taxa e tamanho da FFT.

### 6.3 EVENTS — eventos sonoros

Um detector STA/LTA, como o do modo sísmico do sensor de vibração, trabalha sobre a energia de cada subquadro: média curta (STA, τ = 50 ms) contra média longa (LTA, τ = 10 s). Depois de 2 s de aquecimento, um evento começa quando STA/LTA passa do limiar (padrão 8, isto é, +9 dB sobre o fundo; o console e a TUI o propõem ao escolher o modo). Durante o evento a LTA fica congelada; ele termina quando a razão cai abaixo de 2 (+3 dB) ou após 30 s — neste caso o ruído novo passa a ser o fundo.

A FFT, neste modo, roda só durante eventos e uma vez por segundo (para manter a razão A/Z). O quadro que fechou imediatamente antes do disparo é analisado retroativamente, para que o começo do evento não se perca.

| Classe | Regra | Exemplos |
|---|---|---|
| `IMPACT` | duração < 400 ms e centroide < 2,5 kHz | batida, queda de peça |
| `CRACK` | duração < 400 ms e centroide ≥ 2,5 kHz | estalo, quebra, faísca |
| `TONAL` | duração ≥ 400 ms e planura < 0,1 | alarme, apito, sirene |
| `TRANSIENT` | duração ≥ 400 ms, banda larga | passagem de veículo, partida de máquina |

Para cada evento o relatório traz a classe, o pico, a duração, o centroide e o SEL — a energia A acima do fundo durante o evento, referida a 1 s.
<!-- livro:fim id="acustico-modos" -->

<!-- livro:inicio id="acustico-configuracao" capitulo="Sensor acústico INMP441" -->
## 7. Configuração, calibração e console

### 7.1 Campos da configuração

O sensor acústico usa a mesma mensagem de configuração do sensor de vibração (`Payload_Configuration`), com outro significado para alguns campos:

| Campo | Perfil acústico | Faixa aceita |
|---|---|---|
| `target_mode` | `IDLE`, `LEVEL`, `MACHINE`, `EVENTS` | 0 a 3 |
| `sample_rate_hz` | frequência de WS | 8000, 16000, 24000, 32000, 48000 |
| `window_size` | pontos da FFT | 256, 512, 1024, 2048 |
| `window_type` | janela da FFT | RECT, HANN, HAMMING, FLATTOP, BLACKMAN-HARRIS |
| `stalta_thresh` | limiar do modo: razão STA/LTA em EVENTS, escore z em MACHINE | maior que 1 e até 1000 |
| `calib_gain` | ganho de calibração da pressão | maior que 0,01 e até 100 (±40 dB) |

### 7.2 Calibração

Com um calibrador acústico encaixado no microfone (1 kHz, onde A, C e Z coincidem) por pelo menos três segundos, em qualquer modo de medição:

```text
EDGE> AUTH UNLOCK <token>
EDGE> CAL 94
OK CAL MEASURED_DB=93.12 REFERENCE_DB=94.00 OFFSET_DB=+0.88 GAIN=1.1066
CONFIG_APPLIED MODE=LEVEL ... GAIN=1.1066 ...
```

O firmware usa as três últimas leituras de LZeq e recusa a calibração se elas variarem mais de 0,5 dB (`ERR CAL_UNSTABLE`). O ganho novo é `ganho × 10^((referência − medido)/20)`. Pela rede CAN, a TUI faz o mesmo cálculo com as leituras que recebeu e envia o ganho em uma configuração comum (ação "Calibrar com calibrador acústico").

### 7.3 Console USB

```text
STATUS | GET | VERSION | PING
SET MODE <IDLE|LEVEL|MACHINE|EVENTS>
SET RATE <8000|16000|24000|32000|48000>
SET FFT <256|512|1024|2048>          (sinônimo: WINDOW_SIZE)
SET WINDOW <RECT|HANN|HAMMING|FLATTOP|BLACKMAN>
SET THRESHOLD <valor > 1>            (sinônimo: STALTA)
SET GAIN <valor>
APPLY
CAL <nível de referência em dB SPL>
MEASURE RESET
BASELINE LEARN <5..3600> | BASELINE CLEAR
TELEMETRY ON|OFF|ONCE|FAST|SLOW|PERIOD <ms>
FFT ONCE
DTC | DTC CLEAR
NET | AUTH ... | RESET
```

Leitura é livre; tudo o que altera estado exige `AUTH UNLOCK`, como no sensor de vibração. `STATUS` e `VERSION` trazem `PROFILE=ACOUSTIC`, o chip (`RP2040` ou `RP2350`), a placa, o canal em uso e o estado do microfone. A linha de telemetria tem as mesmas chaves que a Probe 00 imprime (ver *Do Node à TUI*), de modo que a TUI usa um único analisador para as duas origens.
<!-- livro:fim id="acustico-configuracao" -->

<!-- livro:inicio id="acustico-transporte" capitulo="Sensor acústico INMP441" -->
## 8. Telemetria e transporte

### 8.1 Do sensor ao Node

O relatório viaja em `DATA_UP` como `Payload_AcousticStream` (comando `0x21`): 120 bytes de níveis, sinalizações e resultados do modo, seguidos de até 256 magnitudes `float` do espectro médio. Níveis vão em centésimos de dB (`int16`), e `INT16_MIN` marca o que o modo não calcula. Sinalizações:

| Bit | Nome | Significado |
|---|---|---|
| 0x01 | `SPECTRUM_VALID` | ao menos um quadro do segundo passou pela FFT |
| 0x02 | `OVERLOAD` | amostras no fundo de escala |
| 0x04 | `EVENT_ACTIVE` | evento em curso (EVENTS) |
| 0x08 | `BASELINE_READY` | assinatura de referência disponível (MACHINE) |
| 0x10 | `BASELINE_LEARNING` | aprendizado em curso (MACHINE) |
| 0x20 | `ANOMALY` | escore acima do limiar (MACHINE) |
| 0x40 | `DOSE_LIMIT` | dose de 100 % ou LAS acima de 115 dB(A) (LEVEL) |
| 0x80 | `MIC_FAULT` | microfone sem sinal válido |

Os comandos próprios são `CMD_ACOUSTIC_CONTROL` (`0x1C`: reinício da medição, aprendizado e descarte da assinatura) e a resposta `CMD_ACOUSTIC_CONTROL_ACK` (`0x1D`), com o contador do pedido ecoado.

### 8.2 Do Node à TUI

O Node converte o relatório em uma transferência do tipo `7` (`IOC_WD_KIND_ACOUSTIC`) de 108 bytes, sem o cabeçalho, a janela, a taxa pedida e o espectro. A segmentação do plano de dados (6 bytes úteis por quadro, 4 no primeiro) dá 19 quadros CAN por relatório, contra 9 da telemetria de vibração; a um relatório por segundo, um sensor acústico ocupa cerca de 2,4 kbit/s do barramento de 500 kbit/s. O espectro só é encaminhado quando pedido (ação FFT), pelo mesmo caminho da vibração, reduzido a 32–256 faixas.

A Probe 00 imprime:

```text
TEL NODE=2 CHILD=1 SEQ=3 GW_TS_MS=13705 PROFILE=ACOUSTIC MODE=LEVEL ACQ=I2S WIN=1024 RATE_EFF_HZ=16000
    INTERVAL_MS=1000 COVERAGE_PCT=100 CPU_PCT=35 SPECTRUM_VALID=YES OVERLOAD=NO MIC_FAULT=NO
    LAEQ_DB=87.10 LCEQ_DB=91.30 LZEQ_DB=92.20 LZPEAK_DB=103.50 LAF_DB=86.75 LAFMAX_DB=90.00 LAFMIN_DB=84.50
    CREST=4.12 KURT=0.35 PEAK_HZ=1000.0 CENTROID_HZ=1480 FLATNESS=0.1830
    OCT_DB=72.7,78.9,81.8,83.2,84.0,84.7,82.1,76.8,-,-
    LEQ_T_DB=87.90 L10_DB=89.15 L50_DB=87.60 L90_DB=86.50 DOSE_NR15_PCT=0.4 DOSE_NHO01_PCT=0.6 ELAPSED_S=120 DOSE_LIMIT=NO
    BATT_PCT=255 BATT_MV=65535 DTC=0x0000 DTC_COUNT=0
```

(em uma linha só). As chaves de cada modo:

| Modo | Chaves adicionais |
|---|---|
| LEVEL | `LEQ_T_DB L10_DB L50_DB L90_DB DOSE_NR15_PCT DOSE_NHO01_PCT ELAPSED_S DOSE_LIMIT` |
| MACHINE | `TONAL_DB F0_HZ HARMONICS ANOMALY_Z BASELINE=NONE/LEARNING/READY BASELINE_PCT ANOMALY` |
| EVENTS | `EVENTS EVENTS_INTERVAL EVENT_ACTIVE STA_LTA_DB BACKGROUND_DB` e, depois do primeiro evento, `EVENT_CLASS EVENT_PEAK_DB EVENT_SEL_DB EVENT_DURATION_MS EVENT_CENTROID_HZ EVENT_AGE_S` |

Chave ausente significa "não calculado"; em `OCT_DB`, `-` marca a banda acima de fs/2.

### 8.3 Comandos pela rede

| Linha da TUI | Opcode CAN | Efeito |
|---|---|---|
| `CMD ... ACTION=CONFIG MODE=LEVEL\|MACHINE\|EVENTS ...` | `0x10`–`0x1F` | configuração comum; `THRESHOLD` e `FFT_SIZE` são sinônimos de `STALTA` e `WINDOW_SIZE` |
| `CMD ... ACTION=MEASURE_RESET` | `0x21`, ação 1 | reinicia LAeq,T, Ln, doses e contagem de eventos |
| `CMD ... ACTION=BASELINE_LEARN SECONDS=60` | `0x21`, ação 2 | aprende a assinatura (5 a 3600 s) |
| `CMD ... ACTION=BASELINE_CLEAR` | `0x21`, ação 3 | descarta a assinatura |

O argumento 1 de `SET_MODE` leva o perfil a que o nome do modo pertence (1 vibração, 2 acústico, 0 para `IDLE`). Um modo de outro perfil é recusado com `REASON=PROFILE_MISMATCH` (`DETAIL=65533`) e a edição inteira é descartada no `APPLY`; o mesmo vale para o opcode `0x21` enviado a um sensor de vibração. `WIRELESS_CONFIG` ganhou `profile=VIBRATION|ACOUSTIC` no fim da linha; um Node da versão 0.18, que não o informa, continua sendo entendido.
<!-- livro:fim id="acustico-transporte" -->

<!-- livro:inicio id="acustico-diagnostico" capitulo="Sensor acústico INMP441" -->
## 9. Diagnóstico

| DTC | Severidade | Causa | Sintoma (FTB) |
|---|---|---|---|
| `0x1003` | crítica | microfone sem dados válidos no I2S | `0x04` sinal ausente (palavras em zero) ou `0x12` SD preso em nível alto |
| `0x2003` | aviso | sobrecarga acústica (amostras no fundo de escala, ~120 dB SPL) | `0x16` acima da faixa |
| `0x3001` | aviso | blocos de áudio perdidos por atraso do DSP | `0x1F` taxa de amostragem comprometida |
| `0x3002` | crítica | núcleo 1 sem progresso por 2 s | `0x01` falha geral; sem recuperação, o *watchdog* reinicia a placa |
| `0x4006` | aviso | configuração recusada (fora das faixas de *Campos da configuração*) | `0x16` acima da faixa |

Os demais códigos de rede e segurança (`0x4001`–`0x4005`) são os do sensor de vibração, pois o enlace é o mesmo.
<!-- livro:fim id="acustico-diagnostico" -->

<!-- livro:inicio id="acustico-tui" capitulo="Sensor acústico INMP441" -->
## 10. Análises na TUI

A TUI transforma cada relatório em leituras de operador (`Front/pico_tui/acoustics.py`, sem dependência da interface e coberto por testes). A tela do sensor acústico tem quatro blocos — níveis sonoros com barras de oitava e tendência do LAeq, painel do modo, leitura do ambiente e eventos — descritos em [`../interface/tui.md`](../interface/tui.md).

**Exposição (LEVEL).** A partir da dose acumulada e do tempo decorrido:

| Indicador | Cálculo |
|---|---|
| dose projetada para 8 h | dose × 8 h / tempo decorrido |
| tempo até 100 % | tempo × (100 − dose) / dose, no ritmo atual |
| LEX,8h (NEN da NHO-01) | LAeq,T + 10·log10(T / 8 h), com o resto da jornada em silêncio |
| nível médio NR-15 em 8 h | 85 + 16,61·log10(dose projetada / 100) |
| limite diário no nível do momento | 8 h / 2^((L − 85)/q), para L ≥ 85 dB(A) |
| critério da NHO-01 | dose projetada < 50 %: aceitável; 50–80 %: acima do nível de ação; 80–100 %: região de incerteza; > 100 %: acima do limite de exposição |

**Caráter do ruído (todos os modos).**

- *Curva NC*: a curva de critério de ruído (Beranek; ANSI S12.2) tangente às bandas de 63 Hz a 8 kHz, interpolada entre as curvas tabeladas de NC-15 a NC-70, com a banda que a define. Acima da tabela, "acima de NC-70". A banda de 8 kHz pode faltar (fs = 16 kHz); a avaliação segue com as demais e o informa.
- *Conversa*: o nível de interferência na fala (SIL, média das oitavas de 500 Hz a 4 kHz) dá a distância máxima de comunicação confiável — 57,5 dB a 1 m para voz masculina normal, +6 dB por grau de esforço vocal e −6 dB a cada distância dobrada (Beranek).
- *Graves*: LCeq − LAeq acima de 20 dB indica conteúdo forte de baixa frequência, pouco percebido na escala A.
- *Impulsividade*: pico mais de 18 dB acima do nível eficaz (fator de crista acima de 8) ou curtose acima de 3.
- *Variabilidade*: L10 − L90 acima de 10 dB.
- *Caráter espectral*: banda dominante e, com planura alta e centroide acima de 3 kHz, indício de vazamento de ar ou gás.

**Máquina (MACHINE).** Rotação a partir da fundamental (60·f0 rpm a um evento por volta), média e dispersão recentes, faixa de busca do tacômetro na configuração em uso, hipóteses de motor de indução a 60 Hz (polos de 2 a 12 com escorregamento entre 0,5 e 8 %) e identificação do zumbido da rede (60 ou 120 Hz, magnetostrição). O escore de anomalia é mostrado contra o limiar, com tendência.

**Eventos (EVENTS).** Cada fim de evento vira um registro (classe, pico, SEL, duração, centroide); a tela mostra a taxa nos últimos 10 minutos, o evento mais forte e o SEL combinado da sessão (soma energética).

**Ações.** Reiniciar a medição (`z`), aprender a assinatura (`a`, no modo MACHINE), descartar a assinatura, calibrar com calibrador, configurar (modos, taxas de WS e pontos da FFT do INMP441) e espectro em dB SPL (`f`).

As leituras são indicativas: o INMP441 não é um medidor classe 1 ou 2 da IEC 61672, e a exposição ocupacional formal exige equipamento e procedimento normatizados.
<!-- livro:fim id="acustico-tui" -->

<!-- livro:inicio id="acustico-validacao" capitulo="Sensor acústico INMP441" -->
## 11. Validação

Automatizada (sem hardware), executada por `Codigo/scripts/test_native_firmware.sh` e pela suíte da TUI:

| Teste | O que verifica |
|---|---|
| `Codigo/node-audio/tests/test_acoustic_dsp.c` | curvas A e C contra a IEC 61672; FFT contra DFT direta; senoide de 1 kHz a 94 dB SPL lida como 94,0 dB; ponderações aplicadas aos níveis; ruído de banda larga; doses e estatísticas Ln; eventos e sua classificação; fundamental, harmônicos e anomalia; falhas de microfone; política de FFT e lacunas de DMA; limites de configuração (também com AddressSanitizer e UBSan) |
| `Codigo/node-audio/tests/test_audio_config.c` | faixas de configuração do perfil acústico |
| `Front/tests/test_acoustic_firmware_v019.py` | simulação ciclo a ciclo do programa da PIO (palavras de 24 bits alinhadas nos dois canais, leitura só nas bordas de subida); contratos do firmware (dois núcleos, reserva dinâmica de PIO/DMA, errata E13, perfil anunciado, configuração por chip) |
| `Codigo/common/tests/test_ioc_link.c` | codificação e decodificação do relatório de 108 bytes e da configuração com perfil |
| `sim_wireless_link.cpp` | sensor acústico simulado ↔ Node ↔ Probe: relatório nos três modos, espectro, configuração, recusa cruzada de modos, ações e tempo esgotado |
| bancada virtual, cenário `acoustic_sensor` | rede completa com o sensor acústico; a gravação (`Front/replays/acoustic_sensor.log`) alimenta os testes da TUI e `--replay` |
| `Front/tests/test_acoustic_tui_v019.py` | análises, decodificadores, estado a partir da gravação e telas (demonstração e console USB simulado) |

O firmware compila sem avisos para `pico_w` e `pico2_w` (`./Codigo/scripts/build_pico.sh [pico_w|pico2_w] --audio`), também na integração contínua.

Pendente em bancada:

1. Calibrador de 94 dB: leitura e ajuste do ganho, nas duas placas.
2. Ocupação do núcleo 1 e cobertura da FFT para cada taxa e tamanho de FFT, nas duas placas.
3. LAeq e doses contra um medidor de nível sonoro de referência.
4. Tacômetro contra um tacômetro óptico em um motor; escore de anomalia com uma falha provocada (desbalanceamento, folga).
5. Classificação de eventos com sons conhecidos (batida, estalo, apito, passagem).
6. Troca automática de canal com L/R em GND e em VDD; falha com SD desconectado.
<!-- livro:fim id="acustico-validacao" -->

<!-- livro:inicio id="acustico-limitacoes" capitulo="Sensor acústico INMP441" -->
## 12. Limitações e próximos passos

- A resposta do INMP441 não é compensada: abaixo de 60 Hz e acima de 15 kHz os níveis de banda são subestimados.
- O pico mensurável é limitado pela sobrecarga do microfone (120 dB SPL); o limite de 130 dB para ruído de impacto da NR-15 (Anexo 2) não pode ser avaliado.
- A média de Welch não usa sobreposição; com janela de Hann, parte da energia entre quadros pesa menos no espectro (não nos níveis, que vêm do tempo).
- A classificação de eventos é heurística (duração, centroide, planura); um classificador treinado com sons da instalação é o próximo passo natural.
- O espectro reduzido pelo Node para o CAN guarda o máximo de cada grupo de faixas, e a TUI o exibe em dB por coluna: adequado para localizar componentes, não para somar energia.
- O Pico W pode ficar no limite com 48 kHz e FFT de 2048 pontos; a configuração padrão do RP2040 é conservadora até a medição em bancada (ver *Validação*).
<!-- livro:fim id="acustico-limitacoes" -->
