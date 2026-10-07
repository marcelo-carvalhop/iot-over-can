# Modelo de segurança

A segurança atua em duas fronteiras. A TUI autoriza o operador antes do envio e o firmware do sensor direto valida ações mutáveis. A TUI não é considerada a única barreira de segurança.

## Arquivos locais

Build do sensor:

```text
<raiz-do-repositorio>/.env.local
```

TUI:

```text
~/.config/iot-over-can/security.json
```

Preferências visuais ficam separadas e não contêm segredos:

```text
~/.config/iot-over-can/tui.json
```

Em POSIX, `.env.local` e `security.json` devem usar permissão `0600`. Nenhum deles deve ser versionado.

## Provisionamento

```bash
./Codigo/scripts/provision_sensor_security.sh
./Codigo/scripts/provision_sensor_security.sh --check
```

O primeiro comando cria os arquivos quando não existem; o segundo valida sem imprimir os segredos.

Renovação:

```bash
./Codigo/scripts/provision_sensor_security.sh --renew
./Codigo/scripts/build_pico.sh
```

Depois da renovação, o Pico deve ser recompilado e gravado porque o token de administração do firmware faz parte da configuração de build.

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
22 00 ...     eleição CAN
22 10 ...     administração direcionada de Node
22 30 ...     período de liveness
```

`22 20 ...` é consulta e permanece leitura.

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

A segurança atual da associação é operacional; autenticação criptográfica de origem do UUID, do Node responsável e dos comandos CAN ainda é item pendente. O futuro plano de dados não deve reutilizar apenas RSSI ou conhecimento do UUID como prova de identidade.

## Rede

SSID, senha e chaves não ficam fixos no código-fonte público. O firmware recebe credenciais conforme o fluxo de provisionamento e usa DHCP. Controle UDP mutável permanece desabilitado por padrão enquanto o mecanismo de autenticação de rede não estiver fechado.
