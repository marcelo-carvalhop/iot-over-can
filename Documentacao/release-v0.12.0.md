# Baseline v0.12.0 — Security Foundation

Esta entrega consolida a etapa de hardening iniciada a partir da revisão de código de 09/09/2026 e das correções funcionais realizadas na TUI e nos firmwares.

## Escopo consolidado

A versão separa explicitamente a Probe 00 da topologia funcional, trata os módulos CAN como unidades edge com capacidades independentes do papel `LEADER/FOLLOWER`, mantém o sensor local de demonstração `0xAA` como capacidade do próprio módulo e torna a interface de sensores remotos orientada a perfis.

Na fronteira do sensor remoto foram incorporados: identidade UUID64 derivada do dispositivo, validação compartilhada de configuração, autorização de comandos mutáveis no firmware, confirmação de reset, provisionamento de credenciais fora do repositório, DHCP, jitter no mecanismo legado de descoberta e preparação do ciclo de vida do CYW43 para a futura descoberta BLE.

A comunicação CAN direcionada ganhou confirmação explícita de execução. O ACK confirma o resultado operacional, mas ainda não autentica criptograficamente a origem do comando.

## Validação desta finalização

```text
Python compileall                          PASS
Testes Python sem runtime Textual          50 passed, 1 deselected
Teste C nativo config_validation           PASS
Scripts shell (bash -n)                    PASS
MANIFEST.json                              PASS
pyproject.toml                             PASS
Workflow CI YAML                           PASS (quando parser YAML disponível)
Pico SDK build real                        NÃO EXECUTADO LOCALMENTE
PlatformIO build real                      NÃO EXECUTADO LOCALMENTE
```

O item desmarcado da suíte Python depende da camada visual `Textual`, que não está instalada no ambiente de empacotamento. Isso não equivale a uma validação visual completa da TUI.

## Itens deliberadamente abertos

Descoberta BLE distribuída, associação do sensor remoto pelo nó CAN escolhido na TUI, autenticação criptográfica do vínculo wireless, autenticação de origem dos comandos CAN, FIDO2/Yubico OTP real, persistência de configuração e experimento POLLING × DRDY permanecem fora do escopo concluído desta baseline.

O histórico detalhado de problemas, decisões e pendências está em `Documentacao/development/challenges-and-resolutions.md`.
