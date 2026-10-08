# Módulo CAN

Cada Node CAN usa ESP32 + MCP2515 e participa da lógica distribuída do barramento. O firmware é compilado por PlatformIO em `Codigo/node-can/`.

O ID não é editado no código-fonte. Ele é fornecido ao processo de build ou upload:

```bash
./Codigo/scripts/build_esp32_can_node.sh 1
./Codigo/scripts/upload_esp32_can_node.sh 1 /dev/ttyUSB0
```

O papel `LEADER` ou `FOLLOWER` é dinâmico e não define as capacidades do módulo. Capacidades como sensor local e scan BLE pertencem ao hardware/firmware do Node e permanecem disponíveis independentemente de seu papel distribuído.

A função local atual é `DEMO_BYTE`, com valor `0xAA`. Ela valida o transporte de um dado produzido pelo próprio Node. Futuras funções locais podem representar sensores ou atuadores reais e deverão declarar sua interface específica na TUI.

Nodes funcionais inicializam o scanner BLE passivo. O Node 0 é reservado à Probe 00 e não executa scan BLE.

## Arquivos do firmware

| Arquivo | Conteúdo |
|---|---|
| `src/main.ino` | eleição, sinal de presença, ciclo TDMA, tabela de estado, vigilâncias, laço principal |
| `src/comandos.cpp` | comandos seriais e comandos de controle recebidos pelo CAN |
| `src/falhas.cpp` | análise do sensor local e respostas de estado |
| `src/wireless_discovery.cpp` | scanner BLE, associação, política e execução da reassociação |
| `src/wireless_link.cpp` | plano de dados: ponto de acesso, sessão autenticada, transporte no CAN |
| `src/net_metrics.cpp` | medição do barramento (ativa somente na Probe 00) |
| `src/fault_injection.cpp` | ensaios de falha |

## Vigilâncias

| Vigilância | Quem executa | Prazo | Ação |
|---|---|---|---|
| ausência de um seguidor | líder | 5 períodos do sinal de presença | marca o Node como falho; os sensores dele com política automática são reassociados |
| falta do líder | Probe 00 | 4 períodos | pede eleição |
| falta do líder | cada seguidor | 6 períodos, mais 20 ms por posição abaixo do identificador 31 | pede eleição |
| parada do próprio laço | todo Node | mais de um período sem executar | rearma as vigilâncias acima, sem declarar ninguém ausente |

Um seguidor que recebe o sinal de presença de um líder de identificador maior que o do líder que ele segue passa a seguir o de maior identificador. `-D IOT_FOLLOWER_LEADER_WATCHDOG=0` desativa a vigilância do líder pelos seguidores.

Uma falha confirmada pelo líder permanece registrada até o comando de limpar falha (`22 10 ID 44`); não é apagada por eleição nem por reentrada do Node na rede.

## Associação de sensores wireless

Cada Node mantém uma tabela limitada de vínculos wireless. Um pedido de associação recebido pela Probe 00 contém o Node alvo e o UUID64 do sensor. O Node só aceita o vínculo quando o UUID foi observado recentemente pelo scanner BLE. O menor `child_id` livre é alocado e forma o identificador lógico `parent.child`.

O vínculo não depende do papel de líder ou seguidor. Após a confirmação, novos advertisements do mesmo UUID atualizam o lease de presença. O estado passa a `STALE` após 15 s sem advertisement e a `LOST` após 45 s desde a última observação. O reaparecimento do UUID recupera `ONLINE` sem recriar o vínculo. O scanner desabilita filtro de duplicatas e possui watchdog para reiniciar a varredura quando necessário.

`WIRELESS UNBIND` remove explicitamente o vínculo. O Node publica `UNBOUND`; somente essa confirmação autoriza a TUI a remover o filho lógico. Se o sensor continuar anunciando, volta a ser candidato disponível para nova associação.

Na TUI, `F7`/`w` centraliza candidatos, observações por RSSI, associações e desassociações. O melhor RSSI é recomendação, não decisão autônoma.

## Plano de dados dos sensores associados

`wireless_link.cpp` implementa o plano de dados. Ao aceitar um vínculo, o Node:

1. deriva a chave do sensor a partir da chave mestra recebida no build;
2. abre o ponto de acesso `IOC-NN` (canal 1, 6 ou 11 conforme o ID) e passa a anunciar por BLE a oferta assinada para aquele UUID;
3. autentica o sensor por desafio-resposta e mantém a sessão com `PING` a cada 3 s;
4. encaminha ao CAN, no identificador `0x380 + NODE_ID`, a telemetria (uma amostra por período), o espectro sob pedido, DTCs, confirmações e o estado do enlace;
5. executa os comandos recebidos em `0x304`.

O ponto de acesso só existe enquanto há pelo menos um vínculo. Sem a chave mestra (`[WLINK] LINK_KEY=MISSING` no monitor serial), o Node associa sensores normalmente, mas informa `NO_KEY` e não abre ponto de acesso.

A Probe 00 converte `CMD TARGET=NN.CC ...` em quadros `0x304` e remonta as transferências `0x380 + N` em linhas `TEL`, `FRAG`, `DTC`, `ACK`, `WIRELESS_LINK` e `WIRELESS_CONFIG`. Ela não recebe chave e não fala com sensores.

BLE e Wi-Fi juntos não cabem na partição de aplicação padrão; `platformio.ini` usa `min_spiffs.csv`. Formatos e limites estão em [`../arquitetura/plano-de-dados-wireless.md`](../arquitetura/plano-de-dados-wireless.md).

## Reassociação automática

Cada vínculo tem uma política, escolhida pelo operador na associação ou depois: manual (padrão) ou automática. Todos os Nodes mantêm uma tabela com os vínculos publicados e as observações de RSSI que ouvem no barramento; o líder a consulta uma vez por segundo e, para um vínculo com política automática, pede a associação a outro Node quando o responsável sai do ar ou deixa de alcançar o sensor. O pedido do líder usa identificadores próprios (`0x302`/`0x303`) e não desfaz o vínculo anterior: o responsável antigo cede quando ouve o novo publicá-lo. Entre duas decisões para o mesmo sensor passam no mínimo 30 s. A decisão em si não faz entrada nem saída e está em `Codigo/common/ioc_link/ioc_failover.c`, testada no computador.

Dois Nodes com o mesmo sensor resolvem a duplicidade entre si: cede quem não tem sessão autenticada com o sensor, ou quem já não o ouve; quem tem sessão permanece. As regras completas, os prazos e os limites estão em [`../arquitetura/metricas-e-ensaios.md`](../arquitetura/metricas-e-ensaios.md).

## Ensaios de falha

`fault_injection.cpp` trata o comando `0x305` nos Nodes e os comandos `FAULT ...` na Probe 00. Durante `POWER_CYCLE` e `SILENCE` o controlador CAN fica em modo somente escuta (não transmite nem confirma quadros) e o laço principal apenas descarta o que chega, exceto o cancelamento. `-D IOT_FAULT_INJECTION=0` remove o tratamento do comando nos Nodes.

## Alimentação (planejado)

Nada desta seção está implementado. Ela registra o problema observado na bancada e a proposta para tratá-lo.

### Consumo

Valores típicos do ESP32, não medidos nas placas do projeto:

| Situação | Corrente em 5 V |
|---|---|
| Node sem sensor associado: processador e varredura BLE contínua (janela de 50 ms a cada 100 ms) | 100 a 130 mA |
| Node com sensor associado: o ponto de acesso Wi-Fi fica ligado | 150 a 200 mA em média, picos de 300 a 500 mA ao transmitir |
| Partida (calibração do rádio) | picos de 300 a 500 mA |
| Módulo MCP2515 com TJA1050 | 10 a 70 mA, conforme a ocupação do barramento |

Três Nodes somam de 0,4 a 0,6 A em média, com picos acima de 1 A quando coincidem. Um power bank com saída de 1 A não sustenta esses picos: a tensão cai, os ESP32 reiniciam por subtensão e, ao religarem juntos, repetem o pico. Na bancada, use uma fonte de 5 V com 3 A ou mais, cabos curtos e, se as placas dividirem a mesma alimentação, um capacitor eletrolítico de 470 a 1000 µF entre 5 V e GND perto de cada uma.

### Como diagnosticar hoje

No monitor serial (115200), a primeira linha após um reinício informa o motivo: `rst:0xf (BROWNOUT_RST)` ou `Brownout detector was triggered` indicam alimentação; `rst:0xc (SW_CPU_RESET)` com `Guru Meditation` indica falha do firmware. Na TUI, vários Nodes pedindo entrada na rede ao mesmo tempo apontam para uma causa comum, quase sempre a alimentação.

### Monitoramento proposto

Em três níveis, do que não exige hardware ao que exige:

1. **Motivo do reinício, sem hardware adicional.** Na partida, o Node lê `esp_reset_reason()` (alimentação, subtensão, falha do firmware, vigilância, reinício por software) e incrementa um contador de reinícios em memória não volátil. As duas informações vão ao barramento quando ele entra na rede. A Probe 00 as escreve em uma linha própria (por exemplo, `[GW] NODE_BOOT node=2 reason=BROWNOUT boots=7`), e a TUI mostra na tela do módulo o último motivo e quantos reinícios por subtensão houve, abrindo uma intercorrência quando o motivo for subtensão.
2. **Tensão de entrada, com dois resistores.** Um divisor do 5 V para uma entrada do ADC1 (o ADC2 não funciona com o Wi-Fi ligado), lido com `analogReadMilliVolts`. O Node publica a menor tensão de cada janela junto com o seu estado, e a TUI avisa abaixo de cerca de 4,6 V. É um aviso antecipado: o detector de subtensão do ESP32 vigia o 3,3 V e só atua quando o 5 V já caiu bem abaixo disso, por causa da queda no regulador da placa.
3. **Corrente, com um sensor I²C (INA219 ou INA226).** Opcional. Mede o consumo real de cada placa, o que permitiria comparar, com números, o Node com e sem ponto de acesso e o efeito dos ajustes abaixo.

### Ajustes de consumo no firmware

- potência de transmissão menor no ponto de acesso (`WiFi.setTxPower`), já que na bancada o sensor fica perto;
- partida escalonada: cada Node espera um intervalo proporcional ao seu identificador antes de ligar os rádios, para que os picos de calibração não coincidam;
- opção de reduzir o ciclo da varredura BLE quando não há sensor sendo procurado.

Nenhum desses ajustes substitui uma fonte adequada; eles reduzem os picos e a média.

## Persistência

A associação ainda não é persistida em memória não volátil; uma reinicialização do Node remove a tabela local de vínculos e as sessões. Com a política automática, o líder devolve o sensor ao Node quando este volta a observá-lo e nenhum outro o alcança melhor; com a política manual, o operador precisa associar de novo. A persistência será adicionada antes de considerar o mecanismo de associação completo para implantação permanente.
