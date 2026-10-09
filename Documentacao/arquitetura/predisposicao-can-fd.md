# Predisposição para CAN FD

O projeto opera em CAN clássico a 500 kbit/s com MCP2515. Este documento registra o que já foi preparado para uma migração futura a CAN FD, o que essa migração exigiria e a recomendação atual sobre fazê-la.

**Nenhum firmware do projeto usa CAN FD.** O que existe é código de biblioteca, testado no computador, que ainda não está ligado a nenhum quadro.

## 1. O que está preparado

| Item | Arquivo | Estado |
|---|---|---|
| Segmentação com quadros de 8 a 64 bytes | `Codigo/common/ioc_link/ioc_wdata.c` (`ioc_wd_segment_count_ex`, `ioc_wd_build_segment_ex`, `ioc_wd_rx_push_ex`, `ioc_wd_fd_frame_len`) | implementado e testado; as funções de 8 bytes usadas pelo firmware são casos particulares das novas |
| PDU autenticada no modelo do AUTOSAR SecOC | `Codigo/common/ioc_link/ioc_secpdu.c` | implementado e testado; não é chamado pelo firmware |
| Chave para o barramento | `ioc_secpdu_derive_bus_key` (`HMAC(K_master, "ioc-can-v1")`) | implementado; nenhuma chave é distribuída |
| Faixas de identificadores | `Codigo/node-can/include/can_ids.h` | não dependem do tamanho do quadro |

### 1.1 Segmentação

O formato do segmento é o mesmo do CAN clássico; muda quanto cabe em cada quadro. O primeiro segmento leva `tamanho do quadro − 4` bytes e os demais `tamanho do quadro − 2`. O último é completado com zeros até o tamanho válido seguinte de CAN FD (8, 12, 16, 20, 24, 32, 48 ou 64 bytes).

| Mensagem | Bytes | Quadros com 8 bytes | Quadros com 64 bytes |
|---|---|---|---|
| Amostra de telemetria | 50 | 9 | 1 |
| Bloco de espectro (80 faixas) | 177 | 30 | 3 |
| Relatório de enlace | 19 | 4 | 1 |
| Configuração | 21 | 4 | 1 |

### 1.2 PDU autenticada

```text
carga útil (n) | atualidade truncada (1) | MAC truncado (m)

MAC = HMAC-SHA256(chave, identificador do dado (2, big-endian) || carga útil || atualidade completa (8, big-endian))
```

O contador de atualidade completo, de 64 bits, nunca trafega. Cada lado mantém o seu e o receptor o reconstrói a partir do byte menos significativo, dentro de uma janela de 255 valores. Um valor só é aceito se for maior que o último aceito, o que impede repetição. O identificador do dado entra no MAC para que uma PDU válida não possa ser reapresentada em outro identificador.

| Perfil | MAC | Proteção por quadro | Carga útil restante |
|---|---|---|---|
| CAN FD | 8 bytes (64 bits) | 9 bytes | 55 de 64 |
| CAN clássico | 3 bytes (24 bits) | 4 bytes | 4 de 8 |

O perfil clássico existe para comparação. Com 4 bytes úteis por quadro, quase toda mensagem do projeto precisaria de dois quadros, e um MAC de 24 bits é fraco para um contador que cresce devagar.

### 1.3 O que não está preparado

- **Medição do barramento.** `ioc_canbits.c` calcula a duração do quadro clássico. O quadro FD tem outra estrutura: CRC de 17 ou 21 bits, contagem de bits de preenchimento no próprio quadro e duas taxas de bit.
- **Análise de tempo de resposta.** `rta.py` usa a duração do quadro clássico.
- **Driver.** O firmware usa a biblioteca ACAN2515 diretamente.
- **Distribuição da chave do barramento.** A derivação existe; a entrega e a renovação não.

## 2. O que a migração exigiria

1. **Hardware em todos os pontos do barramento.** Um controlador que não fala CAN FD responde a um quadro FD com quadro de erro. Não existe barramento misto: Nodes, Probe 00 e qualquer outro equipamento precisam trocar de controlador (MCP2518FD, por exemplo) e usar transceptores adequados à taxa da fase de dados.
2. **Troca do driver.** `ACAN2515` por um driver do controlador novo, com mensagens de até 64 bytes e configuração da fase de dados.
3. **Ligação das funções `_ex`** em `wireless_link.cpp` e na remontagem da Probe.
4. **Medição e análise** para o quadro FD (seção 1.3).
5. **Nova validação em bancada** de tudo o que já foi validado em CAN clássico: eleição, sinal de presença, associação e plano de dados.

## 3. Recomendação: não migrar agora

A pergunta é se vale a pena adiantar a migração. A avaliação, com os dados que o próprio projeto passou a medir na versão 0.18, é que não.

**O barramento não é o gargalo.** Com três Nodes e um sensor a ocupação medida fica perto de 1 %. A análise de tempo de resposta, com o tráfego observado, comporta da ordem de 160 sensores a uma amostra por segundo mantendo a ocupação abaixo de 40 %, e da ordem de 400 no limite dos prazos. O projeto esbarra antes em outros limites: quatro sensores por ponto de acesso e quatro vínculos por Node.

**A tese do projeto é mais forte em CAN clássico.** O argumento é que a rede legada permanece e a IoT se adapta a ela. Trocar o controlador de todos os equipamentos para acomodar os sensores enfraquece esse argumento. O transporte segmentado em identificadores de menor prioridade é justamente a demonstração de que dá para integrar sem tocar na rede.

**O custo é concentrado e sem ganho intermediário.** Não há migração parcial. Todo o hardware troca de uma vez e tudo o que foi validado em bancada precisa ser validado de novo, perto da data de entrega.

**O que o CAN FD traria de fato** é autenticação de origem no barramento com MAC de 64 bits em um único quadro, transporte sem segmentação para a telemetria e carimbo de tempo no hardware para medir variação abaixo de 1 ms. São ganhos reais, mas nenhum deles resolve um problema que a versão atual tenha.

**Para o TCC, a migração rende mais como experimento do que como pré-requisito.** As métricas, a análise de tempo de resposta e os ensaios desta versão formam a linha de base. Repetir as mesmas medições em CAN FD, com os mesmos cenários, dá um capítulo de comparação com números dos dois lados.

### Quando rever esta recomendação

- a ocupação medida passar de 30 % em operação normal;
- for necessário transmitir espectro de forma contínua, e não sob pedido;
- a autenticação de origem no barramento se tornar requisito, e não apenas risco aceito;
- a análise precisar de variação de período abaixo de 1 ms.

### Alternativa intermediária em CAN clássico

Se a autenticação dos comandos se tornar necessária antes da migração, é possível protegê-los sem trocar hardware: cada comando (`0x300`/`0x301`, `0x304`, `0x305`) passaria a ser seguido de um quadro com atualidade e MAC truncado, usando `ioc_secpdu` no perfil clássico. O custo é um quadro a mais por comando, em um tráfego que é esporádico. Os dados dos sensores continuariam sem autenticação no barramento, protegidos apenas no enlace sem fio. Não está implementado.
