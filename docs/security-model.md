# Modelo de segurança operacional

## Objetivo

A proteção implementada não pretende substituir criptografia industrial, HSM ou autenticação FIDO2 completa. O objetivo desta etapa é criar uma barreira física e operacional coerente com a proposta de sistema desconectado e supervisionado.

## Modos

### presence

```bash
pico-tui --security-mode presence
```

A TUI verifica a presença de uma YubiKey por USB. Em Linux, a verificação principal procura o vendor id USB da Yubico:

```text
1050
```

Se a chave não estiver conectada, comandos mutáveis são bloqueados.

### otp

```bash
pico-tui --security-mode otp
```

O operador desbloqueia temporariamente a TUI com:

```text
:unlock <otp-da-yubikey>
```

Arquivo opcional:

```text
~/.config/pico_tui/security.json
```

Exemplo:

```json
{
  "otp_public_ids": ["cccccccccccc"],
  "unlock_seconds": 300
}
```

Se `otp_public_ids` estiver configurado, a TUI aceita apenas OTPs cujo public id inicial esteja cadastrado.

### off

```bash
pico-tui --security-mode off
```

Desativa a proteção operacional. Útil para desenvolvimento sem YubiKey.

## Interpretação correta

Esta camada é suficiente para demonstrar uma política de operação com chave física: conectar Wi-Fi, limpar DTC, alterar configuração, reiniciar aquisição e iniciar telemetria passam a exigir presença/autorização do operador.

Para segurança criptográfica forte, a próxima evolução recomendada é um desafio-resposta com FIDO2/HMAC, em que o gateway ou a TUI emite um desafio e a YubiKey assina/responde localmente.
