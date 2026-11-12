# Segurança

Este documento descreve a segurança implementada hoje: quem pode fazer o quê, como as chaves são geradas e guardadas, o que está protegido e o que não está. Uma revisão do modelo está em desenvolvimento; quando entrar no projeto, este documento muda com ela.

## 1. Visão geral

A proteção atua em três pontos, de forma independente: a estação não é a única barreira.

| Ponto | O que protege | Como |
|---|---|---|
| estação de operação | comandos que alteram a rede | autorização do operador antes do envio |
| console USB do sensor | comandos que alteram o sensor | sessão de manutenção com token |
| canal de dados sensor ↔ módulo | identidade do sensor e integridade dos dados | chave por dispositivo, desafio-resposta e código de autenticação em cada mensagem |

Além de proteger, a estação registra **quem agiu**: cada alteração e cada autorização de sensor entra no histórico com o nome de quem a fez (seção 5).

O barramento CAN é tratado como domínio confiável: os quadros não têm autenticação de origem (seção 8).

## 2. Chaves

Todas as chaves têm 32 bytes, e todas as derivações usam HMAC-SHA256:

```text
K_master                                         nos módulos e no computador que grava as placas
  ├── K_dev = HMAC(K_master, "ioc-dev-v1" || UUID)     uma por sensor
  └── K_net = HMAC(K_master, "ioc-net-v1")             rede Wi-Fi dos módulos
        └── senha do ponto de acesso do módulo N = hex(HMAC(K_net, "ioc-psk-v1" || N))[0..31]

K_sess = HMAC(K_dev, "ioc-sess-v1" || UUID || N || nonce_sensor || nonce_módulo)     uma por sessão
```

| Onde fica | O que guarda | Se a memória for extraída |
|---|---|---|
| módulo CAN | `K_master` | compromete o canal de todos os sensores; exige renovar tudo |
| sensor gravado com `--sensor` | `K_dev` e `K_net` dele | permite se passar por aquele sensor e entrar na rede Wi-Fi; não revela as chaves dos outros |
| sensor de bancada (sem `--sensor`) | `K_master` | igual a um módulo; use só na bancada |
| Probe 00 e estação | nenhuma chave do canal | — |

`K_net` só dá acesso à rede Wi-Fi do módulo. A identidade de cada sensor é provada com `K_dev`, na aplicação: entrar no ponto de acesso não basta para enviar dados. Sem chave, ou com uma `K_dev` de outro UUID, o sensor ignora as ofertas e nunca liga o Wi-Fi; um módulo sem chave continua descobrindo e associando sensores, mas informa `NO_KEY` e não abre ponto de acesso. Não existe modo de compatibilidade sem autenticação.

## 3. Arquivos e provisionamento

| Arquivo | Conteúdo |
|---|---|
| `.env.local` (raiz do repositório) | token de manutenção e `K_master` |
| `.env.sensor-<UUID>.local` | token de manutenção e as chaves derivadas de um sensor |
| `~/.config/iot-over-can/security.json` | token de manutenção, para a estação |

Nenhum deles entra no repositório (o `.gitignore` cobre `*.local`), e em sistemas POSIX todos têm permissão `0600`. As preferências da estação ficam em outro arquivo (`tui.json`), sem segredos.

```bash
./Codigo/scripts/provision_sensor_security.sh                     # cria as credenciais, se não existirem
./Codigo/scripts/provision_sensor_security.sh --check             # confere, sem mostrar os segredos
./Codigo/scripts/provision_sensor_security.sh --add-link-key      # acrescenta K_master a um .env.local antigo
./Codigo/scripts/provision_sensor_security.sh --sensor <UUID>     # chaves de um sensor
./Codigo/scripts/provision_sensor_security.sh --renew             # troca todos os segredos
```

Os scripts de gravação só leem as credenciais; nenhum outro script gera chaves. Depois de `--renew`, que apaga também os arquivos de sensores, todos os módulos e sensores precisam ser gravados de novo, gerando antes as chaves de cada sensor. A chave `000102…1f` usada nos testes e nas simulações é pública e não deve ir para hardware.

## 4. Autorização na estação

O `SecurityManager` classifica cada comando pelo prefixo antes do envio. São tratados como comandos que alteram a rede:

```text
22 00 ...   eleição               WIRELESS BIND / UNBIND / POLICY
22 10 ...   administração         CMD ...      comandos a sensores
22 30 ...   sinal de presença     FAULT ...    ensaios de falha
SET, APPLY, TELEMETRY, FFT, ACQ, DTC CLEAR, RESET, WIFI, NET WIFI,
MEASURE, BASELINE, CAL                                              console de um sensor
```

`22 20` (consulta), `METRICS` e `TIME` são livres. Os comandos CAN de quatro bytes são classificados pelo valor, não pela grafia: a Probe aceita o mesmo comando escrito de várias formas (`22 0 FF 01`, `0x22 00 FF 01`), e a estação refaz essa leitura antes de autorizar. A regra vale também para o comando manual e para o lançador; esconder uma ação não é autorização.

| Modo | Comportamento |
|---|---|
| `presence` (padrão) | exige a chave física do operador (YubiKey) conectada. É uma política de presença, não uma assinatura de cada comando |
| `otp` | bloqueia tudo até existir verificação criptográfica de OTP/FIDO2 |
| `off` | sem autorização na estação, para a bancada; deve ser pedido explicitamente |

Desligar a autorização na estação não remove as proteções do firmware. A autorização pela chave física e a identificação do operador (seção 5) são independentes e se somam: em `presence`, o comando precisa da chave conectada e, havendo operadores cadastrados, de um operador que tenha entrado.

## 5. Identificação do operador

A autorização da seção 4 responde se a estação pode enviar o comando. Ela não responde quem o pediu. Para o histórico saber quem alterou a rede ou autorizou a entrada de um sensor, a estação identifica a pessoa que a usa e grava o nome dela junto de cada ação. A identificação não depende de chave física: o FIDO2 ainda não existe no projeto, e quando existir entra no mesmo ponto (seção 5.5).

### 5.1 Como funciona

- **Cadastro.** Cada pessoa tem nome de acesso, senha e perfil no arquivo `~/.config/iot-over-can/operators.json`. O arquivo guarda só o resultado do `scrypt` da senha, nunca a senha. Como o `security.json`, ele é recusado se pertencer a outro usuário ou puder ser lido por outros (permissão `0600`).
- **Entrada.** A pessoa tecla `o` (ou `:login`) e informa nome e senha. A senha não aparece na tela e não vai para nenhum registro. Depois de cinco erros seguidos, a entrada espera 30 s, o dobro a cada bloqueio seguinte.
- **Sessão.** Vale enquanto houver teclado, mouse ou comando. Expira após 15 minutos sem atividade (`--operator-idle`, em minutos; `0` desliga a expiração). Trocar a senha, desativar ou remover o operador na CLI encerra a sessão aberta dele em até 5 s, ou no próximo comando.
- **Perfis.** `operator` altera a rede. `observer` acompanha, mas não altera nada: as tentativas são recusadas e registradas.

### 5.2 Modos

| `--operator-mode` | Comportamento |
|---|---|
| `auto` (padrão) | Com operadores cadastrados, o login é exigido para alterar a rede. Sem cadastro, o responsável é o usuário do sistema operacional (`sys:marcelo`): nada muda para quem não configurou, e o histórico já sabe qual conta estava na estação |
| `required` | O login é sempre exigido. Sem operador cadastrado, nada que altere a rede é enviado |
| `off` | Sem identificação nem registro de ações. É o comportamento anterior, para ensaios automatizados |

Consultas (`STATUS`, `GET`, `22 20`) continuam livres: sem login a pessoa pode olhar, não alterar. Um cadastro ilegível ou com permissões erradas bloqueia as alterações em vez de liberá-las: apagar ou estragar o arquivo não desliga o login.

### 5.3 O que fica registrado

Cada ação vira um evento da faixa **AUDIT** do histórico (tecla `h`), com instante, autor, comando sem segredos e resultado.

| Registro | Quando |
|---|---|
| `SESSION_START` | a estação é aberta, com o modo de identificação e o usuário do sistema |
| `LOGIN`, `LOGIN_FAILED`, `LOGOUT` | entrada, tentativa recusada e saída |
| `SESSION_EXPIRED`, `SESSION_REVOKED` | inatividade; senha trocada ou operador desativado |
| `SENSOR_BIND`, `SENSOR_UNBIND`, `SENSOR_POLICY` | autorização de entrada, remoção e política de reassociação de um sensor |
| `SENSOR_CONFIG`, `NETWORK_COMMAND`, `FAULT_COMMAND`, `WIFI_PROVISION` e outros | os demais comandos que alteram a rede |
| `ALARM_LIMITS` | mudança dos limites de alarme de um sensor, com o valor anterior e o novo |
| `COMMAND_DENIED`, `COMMAND_FAILED` | comando recusado (sem login, observador, chave ausente) ou que não chegou a sair |

A resposta do firmware a uma associação (`ASSOCIATING`, `BOUND`, `ONLINE`) é atribuída a quem enviou o comando, por até 2 minutos (`authorized_by`). O que a rede faz sozinha, como um sensor que some ou uma reassociação automática, fica sem autor, que é o retrato correto.

Senhas, tokens e chaves nunca entram nos registros: `AUTH UNLOCK ***`, `NET WIFI PROVISION rede ***`, campos `KEY=`, `TOKEN=` e `PASS=` mascarados. Uma tentativa de entrada recusada grava o nome digitado só se ele existir no cadastro, porque um engano comum é digitar a senha no campo do nome. As mesmas ações vão também para o arquivo da sessão (`logs/session_*.jsonl`, fonte `AUDITORIA`), uma segunda cópia independente do banco.

### 5.4 Cadastro e consulta

```bash
iot-over-can-operators add ana --display "Ana Souza"   # pede a senha no terminal
iot-over-can-operators add visita --role observer
iot-over-can-operators list
iot-over-can-operators passwd ana                      # encerra as sessões abertas dela
iot-over-can-operators disable visita                  # enable, role, remove
iot-over-can-operators check                           # confere formato e permissões
iot-over-can-operators audit --operator ana --since 24h
iot-over-can-operators audit --verify                  # confere a cadeia de hashes
```

No repositório, `./Codigo/scripts/operadores.sh` faz o mesmo no ambiente virtual da estação e na pasta onde o histórico é gravado; use-o no lugar de `iot-over-can-operators` se o comando não estiver no `PATH`. A senha é sempre pedida no terminal, sem eco (ou lida da entrada padrão com `--password-stdin`, para scripts). Nunca vai como argumento: ficaria no histórico do shell e na lista de processos. Tem de 8 a 256 caracteres; uma frase longa vale mais que uma palavra com símbolos.

### 5.5 Garantias e limites

- **Separa pessoas, não resiste a administradores.** A identificação distingue quem fez o quê entre pessoas que dividem a bancada e deixa um rastro. Quem tem acesso de administrador à estação pode trocar o código da TUI, ler a memória do processo, editar o cadastro ou apagar o banco.
- **Cadeia de hashes.** Cada registro de auditoria traz o SHA-256 do anterior mais o seu conteúdo. Editar ou apagar um registro do meio quebra a cadeia, e `audit --verify` aponta onde. Apagar os últimos registros, ou o banco inteiro, não é detectado; para isso existe a segunda cópia no arquivo da sessão.
- **Usuário do sistema é declarado, não verificado.** Sem cadastro, o registro leva a conta do sistema (`sys:...`). Se a conta é compartilhada, o nome identifica a conta, não a pessoa.
- **Só vê o que passa pela estação.** Comandos digitados direto no console USB de um sensor, ou enviados por outra estação, não são registrados por esta. Marcas feitas no painel físico continuam na faixa `OPERATOR`, sem autor: o painel não identifica quem o toca.
- **Tentativas limitadas por processo.** Reiniciar a TUI zera o contador de erros. Quem consegue ler o `operators.json` ainda precisa quebrar o `scrypt`, que é caro de propósito.
- **Ponto de extensão para o FIDO2.** Cada registro guarda o método de identificação (`method`) e se ele foi verificado (`verified`). Um verificador FIDO2 devolve o mesmo `Actor`, com `method="fido2"`, e o histórico, a tela e a CLI continuam como estão ([Integração](integracao.md#identificação-do-operador)).

## 6. Console do sensor

Comandos de leitura não pedem autorização. Comandos que alteram o sensor exigem uma sessão de manutenção:

```text
AUTH STATUS
AUTH UNLOCK <token>
AUTH LOCK
```

A sessão expira sozinha. `RESET` pede confirmação em duas etapas no próprio firmware. A estação busca o token só quando precisa e nunca o grava em registro.

## 7. Canal de dados

**Garantias:**

- o sensor só entra no ponto de acesso de um módulo cuja oferta esteja assinada com `K_dev`;
- sensor e módulo se autenticam mutuamente antes de qualquer dado;
- toda mensagem leva um contador crescente e um código de autenticação de 8 bytes; mensagens adulteradas ou repetidas são descartadas, contadas (`auth_fail`, `replay`) e viram ocorrência de segurança na estação;
- uma sessão nova não derruba a sessão em vigor até ser confirmada;
- a desassociação apaga do sensor o nome e a senha da rede e desliga o Wi-Fi dele.

**Ameaças consideradas:**

| Ameaça | Resultado |
|---|---|
| aparelho sem chave anuncia o UUID de um sensor legítimo | pode ser associado pelo operador, mas não autentica: nenhum dado chega ao barramento |
| oferta BLE forjada para atrair o sensor a outro ponto de acesso | descartada: a assinatura depende de `K_dev` |
| oferta autêntica gravada e repetida | não dá acesso a nada; repetida continuamente, pode atrasar a associação (negação de serviço por rádio) |
| ponto de acesso falso com o nome do módulo | não conhece a senha; mesmo com ela, não produz um desafio válido sem `K_dev` |
| telemetria ou comando forjado na rede Wi-Fi | descartado e contado em `auth_fail` |
| repetição de mensagem autêntica | descartada pelo contador e contada em `replay` |
| HELLO autêntico repetido | não altera a sessão em vigor; repetido no instante certo, ainda pode atrapalhar uma sessão nova (negação de serviço) |
| extração da memória de um sensor | compromete só aquele sensor e a senha do Wi-Fi |
| leitura do tráfego | os dados são autenticados, não cifrados na aplicação; a confidencialidade é a do WPA2 do ponto de acesso |

Interferência em 2,4 GHz interrompe o canal; o protocolo garante que isso não resulte em dados ou comandos falsos.

## 8. Limites conhecidos

- **CAN sem autenticação de origem.** Qualquer equipamento ligado ao barramento pode emitir comandos (`0x080`, `0x304`, `0x305`), pedidos de associação e reassociação ou dados falsos (`0x380 + N`). Com isso pode mover um sensor entre módulos ou deixá-lo sem responsável, mas não consegue ler os dados do sensor nem se passar por ele, porque a sessão continua exigindo `K_dev`. A autenticação no barramento está preparada em biblioteca ([Arquitetura](arquitetura.md#9-preparação-para-can-fd)).
- **Chave mestra em todos os módulos.** A extração da memória de um módulo compromete o canal de todos os sensores. Entregar `K_dev` pela estação na associação eliminaria isso; é a base da proposta de comissionamento sem compilação.
- **Sem elemento seguro.** As chaves ficam na memória flash dos microcontroladores.
- **Código de autenticação de 64 bits**, adequado ao volume e à duração das sessões do projeto, não um parâmetro para uso geral.
- **Chave física verificada só pela presença.** A verificação FIDO2/OTP ainda não existe; o modo `otp` permanece bloqueado. Enquanto isso, quem opera se identifica por senha (seção 5), com os limites descritos ali.

## 9. Ensaios de falha

Os comandos `FAULT` derrubam módulos, desligam pontos de acesso e ocupam o barramento. Na estação eles pedem autorização e confirmação, mostrando o comando. No barramento, o quadro `0x305` não é autenticado. Fora da bancada, compile os módulos com `-D IOT_FAULT_INJECTION=0`, que remove o tratamento do comando, e não deixe a Probe 00 ligada a uma rede em operação: ela continua capaz de gerar carga. Cada ensaio dura no máximo 120 s, e um módulo aceita um ensaio por vez.
