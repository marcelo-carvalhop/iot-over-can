# Arquitetura

Este documento explica como o sistema funciona e por que ele foi feito assim. Os formatos exatos das mensagens estão em [Protocolo](protocolo.md); como modificar e estender o código, em [Integração](integracao.md).

## Glossário

| Termo | Significado |
|---|---|
| **Barramento CAN** | par de fios compartilhado por todos os módulos. Cada mensagem (um **quadro**, de até 8 bytes) vai para todos |
| **Identificador** | número que abre cada quadro. Quando dois módulos transmitem ao mesmo tempo, vence o de identificador menor: o identificador é também a prioridade |
| **Módulo** (Node) | placa ESP32 ligada ao barramento, com um número de 1 a 31 |
| **Probe 00** | placa igual às dos módulos, com o número 0, ligada ao computador. Observa o barramento e leva os comandos da estação |
| **Líder** e **seguidores** | o líder coordena a rede e emite o **sinal de presença**; os demais o seguem. Qualquer módulo pode ser líder |
| **Sensor sem fio** | Pico W (ou Pico 2 W) com um acelerômetro ou um microfone. Também chamado de filho, porque depende de um módulo |
| **UUID** | número de 64 bits que identifica um sensor |
| **Perfil** | o tipo do dispositivo sem fio: `0x01` vibração, `0x02` acústico, `0x04` painel |
| **Vínculo** (associação) | a decisão de que um módulo é o **responsável** por um sensor. O sensor passa a ter o identificador lógico `NN.CC`: número do módulo e número do filho |
| **Canal de dados** (sessão) | a conexão autenticada por Wi-Fi entre o sensor e o seu módulo, por onde passam as medições |
| **Plano de controle** e **plano de dados** | o que decide quem responde por quem (vínculos) e o que transporta as medições (canal de dados) |
| **BLE** | Bluetooth de baixa energia, usado para anúncios curtos sem conexão |

## 1. Visão geral

```text
           anúncio BLE: "sou o sensor X, perfil vibração"
 Sensor ────────────────────────────────────────────────►  todos os módulos ao alcance
        ◄────────────────────────────────────────────────  módulo responsável
           oferta BLE assinada: "o módulo 2 assumiu você"

 Sensor ══ Wi-Fi do módulo 2 ══ desafio-resposta ══ dados autenticados ══► Módulo 2

 Módulo 2 ── CAN, menor prioridade ──► Probe 00 ── USB ──► estação (TUI)
          ◄─ CAN, comando ───────────  Probe 00 ◄─ USB ──  estação (TUI)
```

Cinco princípios orientam as decisões:

1. **A rede CAN não muda para receber os sensores.** As mensagens dos sensores usam os identificadores de menor prioridade e nunca atrasam a coordenação entre os módulos.
2. **O operador decide.** Vários módulos podem ouvir o mesmo sensor; a estação recomenda o melhor sinal, mas a escolha é do operador, e a reassociação automática só acontece com autorização dele.
3. **Só vale o que pode ser provado.** Qualquer aparelho pode anunciar o UUID de um sensor. A identidade é provada no canal de dados, com chave secreta.
4. **A Probe 00 é um instrumento.** A rede elege o líder e se recupera de falhas sem ela.
5. **A estação não finge.** Uma ação só é oferecida se existe um caminho real para entregá-la.

## 2. Rede CAN

O barramento opera em CAN clássico a 500 kbit/s, com o controlador MCP2515. Os identificadores e o formato de cada quadro estão no [mapa de identificadores](protocolo.md#1-mapa-de-identificadores-can).

### Líder e seguidores

O papel de líder é dinâmico e não depende do hardware: qualquer módulo pode assumir. Numa eleição, vence o módulo de maior número entre os candidatos; a chegada de um módulo novo não troca o líder em exercício. O líder emite o sinal de presença, por padrão a cada segundo (de 0,5 a 2 s, ajustável pela estação), e cada módulo ativo tem a sua vez no ciclo para publicar o próprio estado. A eleição leva cerca de 7,5 s: 5 s para os candidatos se apresentarem, 1,5 s de recuperação e 1 s até o primeiro sinal do novo líder.

Se dois líderes coexistem por um instante (por exemplo, um líder que travou e voltou), prevalece o de maior número, tanto entre os líderes quanto para os seguidores.

### Vigilâncias

| O que é vigiado | Quem vigia | Prazo | O que acontece |
|---|---|---|---|
| um seguidor sumiu | o líder | 5 sinais de presença | o módulo é marcado em falha; os sensores dele com reassociação automática passam a outro módulo |
| o líder sumiu | a Probe 00 | 4 sinais | pede uma eleição |
| o líder sumiu | cada seguidor | 6 sinais, mais 20 ms por posição abaixo do número 31 | pede uma eleição; o de maior número reage primeiro |
| o próprio laço do programa parou | cada módulo | mais de um período | rearma as vigilâncias acima, sem declarar ninguém ausente |

Na bancada virtual, com o sinal a cada segundo, a rede volta a ter líder em cerca de 11,5 s com a Probe 00 presente e em cerca de 14 s sem ela. Uma falha confirmada pelo líder fica registrada até o operador limpá-la (comando *Limpar falha*); eleições e a volta do módulo não a apagam.

### Função local

Além de atender sensores sem fio, um módulo pode ter funções próprias: sensores, atuadores ou processamento ligados diretamente a ele. Hoje existe uma função de demonstração, que publica o byte `0xAA` em turnos; ela prova o transporte de um dado produzido pelo próprio módulo e nunca é mostrada como sensor sem fio.

### Vínculos gravados

Cada módulo grava a sua tabela de vínculos na memória não volátil (NVS) a cada mudança: UUID, número do filho, perfil, versão do protocolo e política de cada sensor, protegidos por CRC-32. As sessões não são gravadas; o sensor sempre se autentica de novo.

Depois de um reinício, o módulo lê a tabela e escuta a rede por 12 s, mais que o intervalo em que os vínculos são republicados, antes de retomá-la:

| Durante a espera | Resultado |
|---|---|
| nenhum outro módulo publica o sensor | o vínculo volta, com o mesmo `NN.CC`, e o módulo volta a oferecer o canal de dados |
| outro módulo assumiu o sensor na ausência | o vínculo gravado é descartado |
| o operador desassociou o sensor | o vínculo gravado é descartado |

Uma tabela inválida é ignorada por inteiro, e o operador associa os sensores de novo.

## 3. Sensores sem fio

### Descoberta

O sensor anuncia por BLE o UUID, o perfil e a versão do protocolo. Todos os módulos ao alcance relatam ao barramento o que ouvem, com a intensidade do sinal (RSSI). A estação agrupa os relatos por UUID e recomenda o módulo com melhor sinal recente. A recomendação não é uma decisão.

### Associação

O operador escolhe o módulo responsável; o módulo confere que ouviu o sensor recentemente, aloca o menor número de filho livre e confirma. A estação só mostra o sensor depois da confirmação, nunca antes.

```text
DISCOVERED ─► ASSOCIATING ─► BOUND ─► ONLINE ──15 s sem anúncio──► STALE ──45 s──► LOST
                                        ▲                                            │
                                        └───────────── o anúncio volta ─────────────┘
```

O módulo responsável é a única autoridade sobre a presença do sensor: anúncios BLE e uma sessão ativa contam como presença, e a estação não recalcula esse estado por conta própria. A desassociação só remove o sensor depois que o módulo confirma (`UNBOUND`). Cada módulo republica seus vínculos a cada 10 s, para que uma estação aberta depois reconstrua a rede, e atende até 4 sensores.

### Canal de dados

A associação dispara o canal de dados:

1. **Oferta.** O módulo anuncia por BLE uma oferta assinada com a chave daquele sensor, que diz qual módulo o assumiu. O sensor ignora ofertas que não consegue conferir.
2. **Rede Wi-Fi do módulo.** Cada módulo com sensores mantém um ponto de acesso próprio, `IOC-NN`, nos canais 1, 6 ou 11 conforme o número. Nome e senha da rede são calculados dos dois lados a partir da chave; nunca trafegam pelo ar.
3. **Desafio-resposta.** O sensor se apresenta (HELLO), o módulo responde com um desafio (CHALLENGE) e o sensor confirma (CONFIRM). Cada lado prova ao outro que conhece a chave do sensor, e os dois combinam uma chave de sessão.
4. **Sessão.** Cada mensagem leva um contador crescente e um código de autenticação de 8 bytes (HMAC-SHA256). Mensagens adulteradas ou repetidas são descartadas e contadas.

O módulo verifica a sessão a cada 3 s. Sem tráfego por 10 s, os dois lados a encerram e o módulo volta a oferecer o canal. O sensor que passa 60 s sem conseguir se autenticar desliga o Wi-Fi e espera uma nova oferta. Um HELLO que chega durante uma sessão ativa abre uma troca pendente, sem derrubar a sessão em vigor. O sensor usa o mesmo número aleatório (nonce) em todas as repetições do HELLO de uma tentativa, de modo que uma resposta atrasada continua valendo. A hierarquia de chaves e as ameaças consideradas estão em [Segurança](seguranca.md).

### Do módulo ao barramento

As mensagens do sensor são maiores que um quadro CAN. O módulo as divide em segmentos de até 32 quadros (190 bytes), com CRC-8, no identificador `0x380 + número do módulo`, o de menor prioridade do projeto. O que vai ao barramento:

| Conteúdo | Quando | Tamanho |
|---|---|---|
| telemetria | uma amostra por período (1 s por padrão, de 0,2 a 10 s) | 9 quadros |
| relatório do sensor acústico | um por período | 19 quadros |
| espectro | só quando pedido, reduzido a 16 a 256 faixas | até 4 transferências de 30 quadros |
| alarmes (DTC) e confirmações de comando | na hora | 2 quadros |
| estado do canal de dados | a cada mudança e a cada 5 s | 4 quadros |

O módulo só acrescenta quadros enquanto há menos de 4 esperando no controlador. A fila de transmissão do MCP2515 é única e por ordem de chegada; sem esse limite, um quadro de coordenação ficaria preso atrás de uma rajada de dados. A 500 kbit/s cabem cerca de 3 800 quadros por segundo; a telemetria de um sensor ocupa 9.

### O rádio do módulo

O ESP32 usa o mesmo rádio, e a mesma antena, para BLE e Wi-Fi, e alterna entre os dois. O fabricante classifica a combinação de ponto de acesso, estação conectando e BLE como suportada, mas instável. Por isso:

- a escuta BLE ocupa metade do tempo do rádio (50 ms a cada 100 ms) e cai para um quarto enquanto algum sensor ainda não tem sessão, deixando o rádio para o Wi-Fi na hora em que ele mais precisa;
- com um só sensor esperando, a oferta fica no ar sem ser reiniciada; com vários, ela se reveza entre eles a cada 400 ms;
- a oferta sai do ar quando o HELLO do sensor chega e só volta se a autenticação não terminar em 5 s.

### Etapas da criação do canal

Entre o vínculo e a sessão, o módulo relata ao barramento cada fato que observa: ponto de acesso no ar, oferta anunciada, sensor que entra na rede Wi-Fi e recebe endereço, HELLO conferido ou recusado (com o motivo), desafio sem resposta, sessão aberta ou encerrada. Fatos repetidos são agrupados, no máximo um relato a cada 5 s, e os relatos nunca tomam o lugar da telemetria na fila. A estação monta com eles a seção **Criação do canal de dados** da tela do sensor, com a etapa atual, o tempo de espera, o motivo de uma falha e o que verificar.

### Perfis de sensor

Tudo o que é comum aos sensores segue o mesmo caminho: oferta, sessão, configuração, alarmes, verificação de presença e espectro sob pedido. Muda o conteúdo da telemetria e os comandos próprios de cada perfil. O módulo guarda o perfil informado no HELLO autenticado e recusa um modo de operação do perfil errado: o número 1 é o modo `ROTATING` no sensor de vibração e `LEVEL` no acústico.

No firmware dos sensores, o canal de dados, o BLE, a identidade e os alarmes ficam em `Codigo/node-wifi` e são compartilhados. Cada sensor implementa um conjunto de funções (os ganchos de `edge_app.h`) com o que é só dele. Um sensor novo não precisa copiar a pilha de rede; o roteiro está em [Integração](integracao.md#novo-tipo-de-sensor-sem-fio).

### Sensor de vibração

Um Pico W com o acelerômetro MPU6050 no barramento I²C (GP0 e GP1, endereço `0x68`). Por padrão, analisa janelas de 512 amostras a 1000 Hz e calcula valor eficaz (RMS), curtose, fator de crista, espectro, entropia espectral, velocidade de pico (PPV) e o detector de eventos STA/LTA. Três modos de operação: `ROTATING` (máquina rotativa), `STRUCTURAL` (vibração de estrutura) e `SEISMIC` (eventos).

O Wi-Fi do sensor fica desligado até chegar uma oferta autêntica:

```text
DISABLED ── oferta autêntica ──► DISCOVERY ── desafio válido ──► BOUND
    ▲                                │  ▲                          │
    │   60 s sem autenticar          │  └─ 10 s sem tráfego ───────┘
    └────────────────────────────────┘
    ▲
    └──────────── desassociação (CMD_RELEASE) ────────────── BOUND
```

Com sessão ativa, o sensor ignora ofertas. Quando o módulo deixa de responder, ele desiste da sessão em 10 s e só aceita a oferta de outro módulo depois de 15 s, para que uma oferta antiga repetida por terceiros não o faça alternar de rede.

### Sensor acústico

Um Pico W ou Pico 2 W com o microfone INMP441, que mede nível sonoro, exposição ao ruído, o som de máquinas e eventos sonoros. Tem documento próprio: [Sensor acústico](sensor-acustico.md).

## 4. Reassociação automática

A reassociação é uma autorização dada pelo operador para cada sensor; o padrão é manual. Com a política automática, o líder passa o sensor a outro módulo quando:

| Motivo | Condição |
|---|---|
| `OWNER_LOST` | o responsável foi declarado ausente, ou deixou de republicar o vínculo por 25 s |
| `SENSOR_LOST` | o responsável está ativo, mas não tem sessão com o sensor nem ouve os anúncios dele, e outro módulo o ouve |

Todos os módulos mantêm a mesma tabela, alimentada pelo que já passa no barramento (vínculos republicados e relatos de sinal); só o líder decide, uma vez por segundo. Um líder recém-eleito continua de onde o anterior parou. O destino precisa ter ouvido o sensor nos últimos 12 s, com sinal de pelo menos −90 dBm.

O líder não desfaz o vínculo anterior: ele pede a associação ao novo módulo, e o antigo cede quando ouve o novo publicar o vínculo. Se o pedido se perde, o sensor continua com quem tinha. Outras regras evitam deixar o sensor sem responsável:

| Regra | Motivo |
|---|---|
| no mínimo 30 s entre duas decisões para o mesmo sensor | dá tempo ao sensor de trocar de rede e evita alternância |
| o módulo que aceita e não ouve o sensor em 15 s publica `STALE` | o vínculo não fica parado; o líder pode escolher outro |
| relatos de um módulo declarado ausente são descartados | não se escolhe um destino com base no que ele ouviu antes de sair |
| um líder que volta de uma parada reinicia a contagem e descarta os relatos | não age com uma tabela envelhecida |
| em `SENSOR_LOST`, o destino precisa ouvir o sensor sem interrupção por 6 s, e nada é decidido se o responsável o ouviu depois do último estado publicado | quando um sensor perdido reaparece, todos o ouvem quase ao mesmo tempo; o responsável tem a preferência |

Se dois módulos mantêm o mesmo sensor por um instante, cede quem não tem sessão com ele, ou quem já não o ouve; sem sessão em nenhum dos dois, fica o de maior número.

Na bancada virtual, com o responsável desligado, a rede decide em cerca de 5 s, e a telemetria volta pelo novo módulo em cerca de 25 s. A maior parte desse tempo é espera do sensor (10 s para desistir da sessão e 15 s antes de aceitar outra oferta). Reduzir essas esperas é possível, mas deve ser feito com medição em bancada física.

## 5. Painel de operação

O painel é um dispositivo sem fio como os sensores (perfil `0x04`): associa-se a um módulo e abre o mesmo canal autenticado. Em vez de enviar medições, ele recebe o que o módulo observa no barramento.

- **O contrato é o próprio barramento.** O módulo encaminha ao painel os quadros CAN que vê, inclusive os que ele mesmo transmite. Ao conectar, o painel recebe um retrato (o último quadro de cada estado da rede), a caixa postal (eventos acumulados enquanto estava desconectado) e, depois, o tráfego ao vivo em lotes a cada 200 ms. Carga artificial e espectros não são encaminhados; entram só na conta da ocupação.
- **O painel decide o que mostrar.** Ele usa o mesmo decodificador para todas as fontes e classifica a situação em quatro níveis: rede normal, a rede está tratando, atenção e requer você. LED e avisos sonoros acompanham o nível.
- **O operador responde pelo painel.** Marcas (um instante, uma ciência de alerta com o tempo de reação) sobem pelo canal de dados até o módulo, que as leva ao barramento; a estação as registra na linha do tempo.
- **Lógica separada do hardware.** `PanelApp` reúne o enlace, o retrato, a relevância, as marcas, o LED e os sons, sem depender de placa. Cada hardware implementa a interface `PanelHal` (tela, rádio, relógio, memória, LED e som). A versão atual usa a ESP32-2432S028: LCD de 320×240 com toque, LED RGB e saída de alto-falante, redesenhando só o que mudou e no máximo uma vez por segundo.

## 6. Relógio da rede

Cada módulo e a Probe 00 mantêm a hora UTC a partir do próprio relógio interno e de um ajuste recebido. A hora pode vir, em ordem de preferência, do **terminal** (o computador da estação, pela Probe 00), de um **relógio de tempo real** com bateria (nenhuma placa do projeto tem hoje; o protocolo já o prevê) ou de um **ajuste manual**.

- **Ajuste pelo terminal.** A estação envia a hora do computador à Probe 00 ao conectar e a cada 10 minutos. A Probe a divulga no barramento e todos os módulos se ajustam.
- **Pedido.** Um módulo sem hora a pede 5 s depois de ligar e quando um painel a pede, no máximo uma vez a cada 5 s. Respondem a Probe 00 e o líder, se tiverem.
- **Adoção.** Uma hora de fonte melhor substitui a atual; da mesma fonte, só se diferir mais de 50 ms (2 s no painel). Assim cada ajuste corrige a deriva sem que os módulos fiquem trocando correções.
- **Sem laços.** Receber uma hora nunca provoca uma transmissão, e um pedido gera no máximo uma resposta. Sem terminal na rede, ninguém tem hora: os pedidos ficam sem resposta e tudo funciona com tempos relativos.

A precisão no barramento é de milissegundos. No painel, a hora chega com até alguns décimos de segundo de atraso, o que basta para o relógio do cabeçalho e a hora dos eventos.

## 7. Medição e ensaios

### Medição do barramento

O MCP2515 entrega identificador, tamanho e dados de cada quadro, mas não a duração dele no fio. A Probe 00 reconstrói o restante (o CRC de 15 bits e os bits de preenchimento) e soma a duração exata de cada quadro, os recebidos e os que ela mesma envia. A cada segundo publica a ocupação média, o pico em fatias de 100 ms e os contadores de erro; a cada 5 s, o período e a variação de cada identificador.

Limites: a Probe marca o tempo quando retira o quadro da fila, não quando ele chegou, o que soma cerca de 1 ms de incerteza à variação; ela não vê quadros com erro nem retransmissões, mas os contadores de erro indicam quando isso acontece.

### Tempos de resposta

| Medida | Como é obtida |
|---|---|
| ida e volta entre módulo e sensor | o módulo envia uma verificação a cada 3 s, e o sensor a devolve |
| comando até a confirmação | a Probe anota o envio do comando e a chegada da confirmação |
| intervalo da telemetria e amostras perdidas | a estação acompanha a numeração das amostras, dada pelo módulo |

### Análise de tempo de resposta

A estação calcula, a partir do tráfego medido, o pior tempo de resposta de cada mensagem pelo método de Davis, Burns, Bril e Lukkien (*Controller Area Network (CAN) schedulability analysis: Refuted, revisited and revised*, Real-Time Systems, 2007). As transferências segmentadas entram como rajadas de quadros liberados juntos. Com isso ela estima quantos sensores a mais o barramento comporta: no tráfego da bancada virtual (três módulos e um sensor), da ordem de 400 no limite dos prazos e de 160 mantendo a ocupação abaixo de 40 %.

A análise vale para o tráfego observado, não para o pior caso de projeto. Ela também não considera a fila única do MCP2515, os erros de transmissão e as retransmissões, e infere o tamanho das rajadas, sem contá-lo. Um prazo "não cumprido" pede conferência com os intervalos medidos antes de qualquer conclusão. Na prática, o projeto esbarra antes em outros limites: 4 sensores por módulo.

### Ensaios de falha

A estação provoca falhas controladas e mede a reação da rede a partir das mensagens, sem anotação manual:

| Ensaio | O que o módulo faz |
|---|---|
| queda de módulo | sai do barramento (só escuta), desliga os rádios e reinicia ao final |
| módulo congelado | sai do barramento e para o programa; ao final retoma de onde estava |
| queda do ponto de acesso | encerra as sessões e desliga o Wi-Fi; religa ao final |
| sessões descartadas | esquece as chaves de sessão |
| carga no barramento | a Probe 00 ocupa o barramento no ritmo pedido, com prioridade alta, intermediária ou baixa |

Duração de 1 a 120 s; carga de 1 a 90 %; um ensaio por vez em cada módulo. Os ensaios existem para a bancada: fora dela, compile os módulos com `-D IOT_FAULT_INJECTION=0`.

## 8. Estação de operação

A estação (TUI) é um programa Python em tela de terminal, separado em camadas:

```text
porta serial ─► decodificadores ─► eventos ─► controlador ─► estado ─► telas
                                                    ▲                    │
                     Probe 00 ◄── comandos ◄────────┴──── ações do operador
```

As telas leem cópias do estado, a cada meio segundo, e nunca o alteram diretamente. A estação funciona sem hardware (demonstração, reprodução de gravações, laboratório virtual), grava cada sessão real num arquivo que pode ser reproduzido depois e guarda um histórico de eventos em SQLite, com capturas do contexto em torno de cada ocorrência importante. As telas estão em [TUI](tui.md).

## 9. Preparação para CAN FD

O projeto usa CAN clássico. Parte do caminho para CAN FD, com quadros de até 64 bytes, já está pronta em biblioteca, testada no computador e ainda não usada pelo firmware:

| Item | Onde |
|---|---|
| segmentação em quadros de 8 a 64 bytes | `ioc_wdata.c` (funções `_ex`) |
| mensagem autenticada para o barramento, no modelo do AUTOSAR SecOC: atualidade truncada e código de autenticação de 8 bytes | `ioc_secpdu.c` |
| derivação de uma chave para o barramento | `ioc_secpdu_derive_bus_key` |

A migração exigiria trocar o controlador e o transceptor de **todas** as placas ao mesmo tempo (um controlador clássico responde a um quadro FD com erro), trocar o driver, adaptar a medição do barramento e a análise de tempo de resposta e validar de novo tudo o que já funciona.

**Recomendação: não migrar agora.** A ocupação medida fica perto de 1 % e o barramento não é o gargalo. E o propósito do projeto fica mais claro em CAN clássico: integrar sensores sem fio a uma rede existente sem mexer nela. Vale rever a decisão se a ocupação passar de 30 % em operação normal, se o espectro precisar ser transmitido continuamente, se a autenticação de origem no barramento virar requisito ou se for preciso medir variação de período abaixo de 1 ms (o MCP2518FD tem carimbo de tempo no hardware).

Se a autenticação dos comandos for necessária antes disso, há uma alternativa em CAN clássico: cada comando seria seguido de um quadro com atualidade e código de autenticação truncado (`ioc_secpdu` no perfil clássico). Não está implementada.

## 10. Limites conhecidos e evoluções previstas

| Limite | Situação |
|---|---|
| os quadros CAN não têm autenticação de origem | o canal de dados termina autenticado no módulo; o barramento é tratado como domínio confiável. Preparação em biblioteca (seção 9) |
| a chave mestra fica em todos os módulos e as chaves ficam na memória flash, sem elemento seguro | ver [Segurança](seguranca.md) |
| a reassociação leva cerca de 25 s, quase todos de espera do sensor | ajustar as esperas com medição em bancada física |
| um módulo isolado do barramento, com o Wi-Fi ligado, mantém os sensores e retém os dados | detecção do isolamento pelo próprio módulo, a fazer |
| o consumo de cada módulo não é monitorado | proposta: registrar o motivo de cada reinício e contá-los, medir a tensão de entrada com um divisor resistivo e, opcionalmente, a corrente com um INA219 |
| a chave do sensor entra no firmware na gravação | proposta de comissionamento sem compilação: firmware único, código de instalação na etiqueta e chave entregue pela rede depois da aprovação do operador ([proposta](historico/proposta-comissionamento-de-sensores.md)) |
| a chave física do operador é verificada só pela presença no USB | verificação FIDO2/OTP, a fazer |
| a bancada virtual não reproduz o rádio | complementos possíveis, de prioridade baixa: ponte com o barramento virtual do Linux (SocketCAN `vcan`), GNU Octave para análise, OMNeT++ com FiCo4OMNeT para estudos de escala |
