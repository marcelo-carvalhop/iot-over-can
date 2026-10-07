# Arquitetura

## Elementos físicos

A **Probe 00** é instrumentação externa à lógica distribuída. Conecta o barramento CAN ao computador por USB, observa frames e injeta comandos de bancada. Não participa da eleição, não é líder/seguidor e não possui função sensorial local.

Os **Nodes CAN** são módulos ESP32 + MCP2515. O papel distribuído (`LEADER` ou `FOLLOWER`) é independente das capacidades físicas. Um Node pode executar função local, observar BLE e assumir responsabilidade por sensores wireless.

O **sensor wireless** é um Raspberry Pi Pico W com MPU6050. Ele executa aquisição, DSP, diagnóstico, advertising BLE e infraestrutura de rede local.

```text
                 BLE advertising
Pico W  ─────────────────────────────► Nodes CAN
        ◄═════ Wi-Fi/UDP autenticado ═► Node responsável
                                           │
                                           │ CAN clássico 500 kbit/s
                                           ▼
                                        Probe 00
                                           │ USB
                                           ▼
                                          TUI
```

## Domínios de comunicação

O CAN coordena os módulos funcionais. BLE transporta identidade, perfil, versão de protocolo e presença do sensor wireless, além da oferta de vínculo que o Node responsável dirige ao sensor. A associação atribui um UUID a um Node responsável; não cria, por si só, canal de telemetria.

O plano de dados Pico W ↔ Node CAN é uma camada separada, sobre Wi-Fi/UDP, com sessão autenticada por HMAC-SHA256. Ele só existe enquanto o Node informa o estado `SECURE` para o filho; a TUI distingue os dois e não representa associação como se fosse plano de dados. O desenho completo está em [`plano-de-dados-wireless.md`](plano-de-dados-wireless.md).

## Liderança e liveness CAN

Nodes funcionais podem assumir `LEADER` ou `FOLLOWER`. O líder publica presença; ausência dentro do lease leva à detecção de falha e pode iniciar eleição. A Probe 00 é excluída desse mecanismo.

A chegada de um Node não substitui automaticamente o líder atual. Entre candidatos elegíveis, o firmware usa o critério implementado no protocolo de eleição.

## Funções locais dos Nodes

`DEMO_BYTE=0xAA` é uma função local do Node CAN. Não representa um sensor wireless. Funções locais futuras podem ser sensores, atuadores ou processamento e devem declarar capacidade, estado, dados e interface apropriados.

## Descoberta e associação wireless

O Pico anuncia:

```text
UUID64 + perfil + versão de protocolo
```

Cada Node ao alcance mantém sua própria observação com RSSI e a envia pelo CAN. A TUI agrupa observações pelo UUID. O melhor RSSI é recomendação, não decisão automática.

Fluxo:

```text
DISCOVERED
   │ operador escolhe Node
   ▼
ASSOCIATING
   ▼
BOUND
   │ novo advertisement
   ▼
ONLINE
   │ 15 s sem advertisement
   ▼
STALE
   │ 45 s desde o último advertisement
   ▼
LOST
   │ advertisement retorna
   └──────────────────────────────► ONLINE
```

O Node selecionado aloca o `child_id`; o filho lógico passa a ser `parent.child`. A TUI cria/remove o sensor lógico em resposta ao estado publicado pelo Node, não antecipadamente.

`UNBIND` gera `UNBOUND`; somente então o domínio remove o filho. Se o UUID continuar anunciando, volta ao conjunto de candidatos disponíveis.

O Node CAN responsável é a única autoridade de liveness do vínculo. A TUI não calcula `STALE/LOST` com base na idade local da última mensagem de associação.

## Arquitetura da TUI

A TUI usa separação entre apresentação, domínio e protocolo:

```text
Textual Screens / Dialogs / Widgets
            │
            ▼
     PicoTuiApp actions
            │
            ▼
   DomainController / StateStore
            ▲
            │
DecoderRouter ← SerialClient ← Probe 00 ou Pico W direto
```

Apresentação:

- `app.py`: ciclo de vida, navegação, autorização e tradução das ações;
- `screens.py`: Início, Módulo, Sensor, Comandos, Rede, Mensagens, Sensores sem fio, FFT e Ajuda;
- `dialogs.py`: conexão, confirmação, parâmetros, configuração, alvo e escolha de Node wireless;
- `widgets.py`: componentes responsivos;
- `presentation.py`: condição e rótulos;
- `command_catalog.py`: descrição declarativa das ações;
- `preferences.py`: preferências não sensíveis.

Domínio:

- `core/models.py`: estado de rede, módulos, sensores, candidatos e intercorrências;
- `core/state_store.py`: mutações sincronizadas, snapshots e freshness;
- `services/controller.py`: converte eventos de protocolo em estado.

Protocolo e transporte permanecem separados da apresentação.

## Atualização visual

Apenas a tela visível recebe `refresh_view`. O app obtém snapshots enxutos com histórico e frames limitados. Widgets comparam o conteúdo antes de redesenhar, e listas preservam o item realçado quando apenas os textos mudam.

A navegação é por telas cheias, não por uma grade fixa de painéis. Isso permite que 48 colunas usem a mesma informação essencial que um terminal largo, empilhando seções em vez de truncá-las.

## Plano de controle versus plano de dados

Essa distinção é obrigatória para interpretar o estado atual:

| Função | Situação |
|---|---|
| detectar Pico por BLE | implementado |
| transportar UUID/perfil/RSSI pelo CAN | implementado |
| escolher Node responsável | implementado |
| criar/remover filho lógico | implementado |
| liveness do vínculo | implementado |
| oferta de vínculo e sessão autenticada Pico ↔ Node | implementado; validação em bancada pendente |
| telemetria Pico → Node → CAN → TUI | implementado; validação em bancada pendente |
| comando TUI → CAN → Node → Pico | implementado; validação em bancada pendente |
| persistência do vínculo após reinício do Node | pendente |

A interface filtra ações conforme o estado informado pelo Node. Um filho associado aparece e pode ser desassociado; telemetria, FFT e configuração via Node só são oferecidas enquanto o plano de dados está em `SECURE`.
