# Segurança

Este documento descreve a segurança implementada hoje: quem pode fazer o quê, como as chaves são geradas e guardadas, o que está protegido e o que não está. Uma revisão do modelo está em desenvolvimento; quando entrar no projeto, este documento muda com ela.

## 1. Visão geral

A proteção atua em três pontos, de forma independente: a estação não é a única barreira.

| Ponto | O que protege | Como |
|---|---|---|
| estação de operação | comandos que alteram a rede | autorização do operador antes do envio |
| console USB do sensor | comandos que alteram o sensor | sessão de manutenção com token |
| canal de dados sensor ↔ módulo | identidade do sensor e integridade dos dados | chave por dispositivo, desafio-resposta e código de autenticação em cada mensagem |

O barramento CAN é tratado como domínio confiável: os quadros não têm autenticação de origem (seção 7).

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

Desligar a autorização na estação não remove as proteções do firmware.

## 5. Console do sensor

Comandos de leitura não pedem autorização. Comandos que alteram o sensor exigem uma sessão de manutenção:

```text
AUTH STATUS
AUTH UNLOCK <token>
AUTH LOCK
```

A sessão expira sozinha. `RESET` pede confirmação em duas etapas no próprio firmware. A estação busca o token só quando precisa e nunca o grava em registro.

## 6. Canal de dados

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

## 7. Limites conhecidos

- **CAN sem autenticação de origem.** Qualquer equipamento ligado ao barramento pode emitir comandos (`0x080`, `0x304`, `0x305`), pedidos de associação e reassociação ou dados falsos (`0x380 + N`). Com isso pode mover um sensor entre módulos ou deixá-lo sem responsável, mas não consegue ler os dados do sensor nem se passar por ele, porque a sessão continua exigindo `K_dev`. A autenticação no barramento está preparada em biblioteca ([Arquitetura](arquitetura.md#9-preparação-para-can-fd)).
- **Chave mestra em todos os módulos.** A extração da memória de um módulo compromete o canal de todos os sensores. Entregar `K_dev` pela estação na associação eliminaria isso; é a base da proposta de comissionamento sem compilação.
- **Sem elemento seguro.** As chaves ficam na memória flash dos microcontroladores.
- **Código de autenticação de 64 bits**, adequado ao volume e à duração das sessões do projeto, não um parâmetro para uso geral.
- **Chave física verificada só pela presença.** A verificação FIDO2/OTP ainda não existe; o modo `otp` permanece bloqueado.

## 8. Ensaios de falha

Os comandos `FAULT` derrubam módulos, desligam pontos de acesso e ocupam o barramento. Na estação eles pedem autorização e confirmação, mostrando o comando. No barramento, o quadro `0x305` não é autenticado. Fora da bancada, compile os módulos com `-D IOT_FAULT_INJECTION=0`, que remove o tratamento do comando, e não deixe a Probe 00 ligada a uma rede em operação: ela continua capaz de gerar carga. Cada ensaio dura no máximo 120 s, e um módulo aceita um ensaio por vez.
