# Modelo de segurança

A segurança atua em três fronteiras. A TUI autoriza o operador antes do envio; o firmware do sensor valida ações mutáveis recebidas pelo console USB; e o enlace entre o sensor e o Node CAN responsável é autenticado por HMAC-SHA256 com chave por dispositivo. A TUI não é considerada a única barreira de segurança.

## Arquivos locais

Build dos firmwares:

```text
<raiz-do-repositorio>/.env.local                    token de manutenção + chave mestra do enlace
<raiz-do-repositorio>/.env.sensor-<UUID>.local      token de manutenção + chaves derivadas de um sensor
```

TUI:

```text
~/.config/iot-over-can/security.json
```

Preferências visuais ficam separadas e não contêm segredos:

```text
~/.config/iot-over-can/tui.json
```

Em POSIX, `.env.local`, `.env.sensor-*.local` e `security.json` devem usar permissão `0600`. Nenhum deles deve ser versionado; o `.gitignore` cobre `*.local`.

## Provisionamento

```bash
./Codigo/scripts/provision_sensor_security.sh
./Codigo/scripts/provision_sensor_security.sh --check
```

O primeiro comando cria os arquivos quando não existem; o segundo valida sem imprimir os segredos.

Instalações anteriores à chave de enlace a acrescentam sem trocar o token de manutenção já gravado nos sensores:

```bash
./Codigo/scripts/provision_sensor_security.sh --add-link-key
```

Chaves de um sensor (o UUID aparece em `VERSION` no console do sensor e na tela Sensores sem fio):

```bash
./Codigo/scripts/provision_sensor_security.sh --sensor E6616408432B6F39
./Codigo/scripts/build_pico.sh --sensor E6616408432B6F39
```

Os Nodes CAN recebem a chave mestra no build; os scripts `build_esp32_can_node.sh` e `upload_esp32_can_node.sh` a leem de `.env.local`. A Probe 00 não recebe chave.

Renovação:

```bash
./Codigo/scripts/provision_sensor_security.sh --renew
./Codigo/scripts/build_pico.sh
```

A renovação troca também a chave mestra do enlace e apaga os arquivos `.env.sensor-*.local`. Depois dela, os Nodes CAN e todos os sensores devem ser recompilados e gravados, gerando antes as chaves de cada sensor com `--sensor`.

## Classificação de comandos na TUI

`SecurityManager` classifica por prefixo. Entre os comandos mutáveis estão:

```text
SET ...
APPLY
DTC CLEAR
RESET
NET WIFI ON/OFF/PROVISION/CLEAR
WIFI ...
CMD ...
WIRELESS BIND ...
WIRELESS UNBIND ...
WIRELESS POLICY ...
FAULT ...     ensaios de injeção de falhas
22 00 ...     eleição CAN
22 10 ...     administração direcionada de Node
22 30 ...     período de liveness
```

`22 20 ...` é consulta e permanece leitura. `METRICS ON|OFF|RESET` altera apenas o que a Probe 00 escreve na serial e não é classificado como mutável.

Os comandos CAN de quatro bytes são classificados pelo valor, e não pela grafia. A Probe 00 os lê com `sscanf("%hhx %hhx %hhx %hhx")`, que aceita o mesmo comando escrito de várias formas (`22 0 FF 01`, `0x22 00 FF 01`, dígitos a mais, sinal). A TUI refaz essa leitura e autoriza pela forma canônica; até a versão 0.17 essas variações passavam sem autorização.

Essa classificação é aplicada também quando o operador usa o campo de comando manual. O fato de uma ação estar escondida ou desabilitada visualmente não é autorização.

## Presença física

O modo padrão `presence` pode usar uma YubiKey como indicação de presença do operador. O mecanismo atual verifica o dispositivo USB esperado e é uma política operacional, não uma assinatura criptográfica de cada comando.

O modo `otp` permanece fail-closed até existir verificação OTP/FIDO real. Validação apenas por formato, tamanho ou prefixo de token não é aceita como autenticação.

`--security-mode off` é destinado à bancada e deve ser explícito. Desativar a camada da TUI não remove proteções implementadas no firmware.

## Autorização do Pico W em conexão direta

Comandos de leitura não exigem sessão de manutenção. Comandos mutáveis usam:

```text
AUTH STATUS
AUTH UNLOCK <token-64-bit>
AUTH LOCK
```

A sessão expira. A TUI obtém `device_admin_token` somente quando necessário e nunca deve registrar o valor no EventLog/JSONL. `RESET` mantém confirmação em duas etapas no firmware.

## Associação wireless

`WIRELESS BIND` e `WIRELESS UNBIND` alteram responsabilidade distribuída e, por isso, são tratados como mutáveis pela TUI. A interface apresenta confirmação antes da ação e só atualiza a topologia depois da confirmação do Node (`ASSOCIATING/BOUND/...` ou `UNBOUND`).

O UUID anunciado por BLE é apenas um identificador: qualquer dispositivo pode anunciá-lo e ser associado pelo operador. A prova de identidade acontece no plano de dados.

### Reassociação automática

A reassociação a outro Node é uma autorização por sensor, dada pelo operador (`WIRELESS BIND ... AUTO` ou `WIRELESS POLICY`). O padrão é manual. Com a política automática, quem decide é o líder da rede, a partir de informações que trafegam no barramento sem autenticação: o estado de vínculo publicado por cada Node e as observações de RSSI.

Consequências, com o barramento CAN no estado atual (sem autenticação de origem):

- quem consegue escrever no barramento pode forjar um pedido de reassociação ou o silêncio de um Node, e com isso mover um sensor de um Node para outro ou deixá-lo sem responsável;
- não consegue ler os dados do sensor nem se passar por ele: a sessão entre sensor e Node continua exigindo a chave do dispositivo, e um Node só abre sessão com um sensor que prove conhecê-la;
- o efeito máximo é, portanto, indisponibilidade do sensor, o mesmo que esse atacante já obteria ocupando o barramento.

A política manual elimina a decisão automática, não o risco de fundo. A autenticação dos comandos no barramento está preparada em `ioc_secpdu` e descrita em [`arquitetura/predisposicao-can-fd.md`](arquitetura/predisposicao-can-fd.md).

## Ensaios de falha

Os comandos `FAULT ...` derrubam Nodes, desligam pontos de acesso e ocupam o barramento. São recursos de bancada.

- Na TUI são mutáveis: passam pelo `SecurityManager`, pedem confirmação e mostram o comando que será enviado.
- No barramento, o quadro `0x305` não é autenticado. Um equipamento ligado ao CAN pode enviá-lo.
- Fora da bancada, os Nodes devem ser compilados com `-D IOT_FAULT_INJECTION=0`, que remove o tratamento do comando. A Probe 00 continua capaz de gerar carga no barramento (`FAULT LOAD`); ela é um equipamento de instrumentação e não deve permanecer ligada a uma rede em operação.
- A duração é limitada a 120 s e um Node aceita um ensaio por vez.

## Enlace sensor ↔ Node CAN

Hierarquia de chaves (32 bytes, HMAC-SHA256):

```text
K_master                                   Nodes CAN e máquina de provisionamento
  ├── K_dev = HMAC(K_master, "ioc-dev-v1" || UUID64)     uma por sensor
  └── K_net = HMAC(K_master, "ioc-net-v1")               senha do ponto de acesso de cada Node
K_sess = HMAC(K_dev, "ioc-sess-v1" || UUID64 || node || nonce_sensor || nonce_node)
```

Garantias:

- o sensor só entra no ponto de acesso de um Node cuja oferta BLE esteja assinada com `K_dev`;
- sensor e Node se autenticam mutuamente por desafio-resposta antes de qualquer dado;
- todo datagrama carrega contador e código de autenticação; adulteração e repetição são descartadas e contadas (`auth_fail`, `replay`), e a TUI as registra como intercorrência de segurança;
- um sensor compilado com `--sensor` não contém a chave mestra;
- sem chave, sensor e Node mantêm o plano de dados desligado; não há modo de compatibilidade sem autenticação.

Limites:

- os dados são autenticados, não cifrados na camada de aplicação; a confidencialidade é a do WPA2 do ponto de acesso;
- comandos e dados no barramento CAN não têm autenticação de origem: o enlace autenticado termina no Node;
- a chave mestra está presente em todos os Nodes funcionais;
- as chaves ficam na memória flash dos microcontroladores, sem elemento seguro;
- a tag de autenticação tem 64 bits.

O modelo de ameaças completo está em [`arquitetura/plano-de-dados-wireless.md`](arquitetura/plano-de-dados-wireless.md).

## Rede

A chave do sensor ainda entra no binário dele no momento da compilação. A proposta para entregá-la pela rede, depois de o operador provar a posse do sensor com um código de instalação, está em [`arquitetura/comissionamento-de-sensores.md`](arquitetura/comissionamento-de-sensores.md).

SSID, senha e chaves não ficam fixos no código-fonte público. O SSID (`IOC-NN`) e a senha do ponto de acesso de cada Node são derivados de `K_net` nos dois lados e não trafegam pelo ar. O sensor usa DHCP. O Wi-Fi do sensor permanece desligado até uma oferta autêntica, e é desligado novamente na desassociação.

O CLAIM por chave compartilhada de 64 bits e a opção de build `EDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL` do protocolo `0x05` foram removidos. `NET WIFI PROVISION` continua no console USB para diagnóstico de rádio em bancada; sem a chave de enlace, a associação a um ponto de acesso qualquer não abre sessão.
