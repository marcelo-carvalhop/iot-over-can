# Modelo de segurança operacional — baseline v0.12

A segurança passa a ser aplicada em duas fronteiras distintas: a TUI controla o que o operador pode solicitar e o firmware do sensor controla o que o dispositivo realmente executa. A TUI deixa de ser o único portão de autorização.

## TUI

O modo padrão continua sendo `presence`. Em Linux, a presença da YubiKey é detectada pelo Vendor ID USB `0x1050`; descrições textuais como `YubiKey`, `Yubico` ou HWID não são mais aceitas como prova de presença.

O arquivo de configuração passa a ser:

```text
~/.config/iot-over-can/security.json
```

Em sistemas POSIX ele deve pertencer ao usuário atual e usar permissões `0600`. Erro de leitura, JSON inválido, proprietário incorreto ou permissões excessivas deixam a política em `SEC=CONFIG_ERROR`; não há fallback silencioso para um modo menos seguro.

O modo `otp` está deliberadamente fail-closed nesta baseline. A validação antiga por tamanho/prefixo não era autenticação criptográfica. Ele só deverá ser reativado quando houver um verificador Yubico OTP real ou FIDO2.

`--security-mode off` continua disponível para bancada, mas a TUI exibe aviso persistente no início da sessão. Desligar a política da TUI não desliga a autorização implementada no firmware.

## Autorização no firmware do Pico

Comandos de leitura continuam disponíveis pela USB CDC. Comandos que alteram estado exigem uma sessão local de manutenção:

```text
AUTH STATUS
AUTH UNLOCK <token-64-bit>
AUTH LOCK
```

O token não existe no código-fonte. Ele é fornecido no build por `EDGE_SERIAL_ADMIN_TOKEN`. Valor `0` mantém todos os comandos mutáveis bloqueados.

A TUI lê `device_admin_token` do `security.json` e, depois de autorizar o operador, abre a sessão de manutenção do firmware sem registrar o segredo no EventLog.

`RESET` passou a exigir duas etapas:

```text
RESET
RESET CONFIRM
```

A segunda etapa deve ocorrer em até 10 segundos.

## Provisionamento local

Use uma vez:

```bash
./scripts/provision_sensor_security.sh
```

O script gera dois valores independentes: um token de manutenção serial e uma chave temporária para compatibilidade do handshake UDP legado. Ele grava `.env.local` no repositório de trabalho e o `security.json` do usuário com permissões `0600`. `.env.local` é ignorado pelo Git.

Depois:

```bash
./scripts/build_pico.sh
```

## UDP legado

O caminho UDP anterior ainda existe para permitir evolução incremental, mas não é considerado o mecanismo final de admissão wireless. Nesta baseline:

- a chave fixa foi removida do repositório;
- `session_token` passou para 64 bits;
- comandos de controle recebem contador monotônico para rejeitar replay simples;
- datagramas de tamanho inesperado são descartados antes do parse;
- configurações usam a mesma função de validação do console serial;
- mutações UDP legadas ficam desabilitadas por padrão por `EDGE_ALLOW_LEGACY_INSECURE_UDP_CONTROL=0`;
- incompatibilidade de protocolo, rejeição de credencial, replay e configuração inválida possuem DTCs distintos.

Esse contador não substitui MAC/HMAC: um atacante capaz de forjar tráfego ainda poderia fabricar um contador novo. O transporte definitivo deverá usar autenticação criptográfica de payload.

## Wireless alvo

A arquitetura final não usa um gateway Wi-Fi central. O sensor anuncia sua presença por BLE; vários nós CAN podem detectá-lo e reportar RSSI pela rede. O operador escolhe o nó de associação pela TUI. Só então esse nó provisiona a sessão Wi-Fi do sensor e passa a representá-lo como filho lógico. O código Wi-Fi legado foi alterado para receber credenciais em runtime e usar endereço dinâmico, preparando essa transição. Ao desligar Wi-Fi, a baseline desabilita apenas a interface STA e mantém o CYW43 inicializado, evitando uma futura disputa de ciclo de vida quando o BLE advertising for adicionado.
