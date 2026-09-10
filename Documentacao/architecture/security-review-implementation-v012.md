# Implementação dos achados da revisão de código — v0.12

Esta revisão converte os achados de maior risco do relatório de 09/09/2026 em mudanças de código sem confundir a compatibilidade UDP existente com a arquitetura wireless definitiva.

## Implementado

A validação de `Payload_Configuration` foi centralizada em `config_validation.c/.h`. Serial, rede, DSP e `main.c` passam pela mesma política. `sample_rate_hz` precisa ser finito e estar entre 4 e 1000 Hz antes de qualquer conversão para `uint32_t`, eliminando a dependência do clamp tardio do driver MPU6050.

O identificador fixo do Pico foi removido. `device_identity.c` obtém o identificador único de 64 bits da flash através de `pico_unique_id`. O protocolo foi elevado para `0x05` e passa a transportar UUID64.

A serial possui autorização no firmware e `RESET` de duas etapas. A TUI mantém seu checkpoint de autorização, mas agora abre explicitamente uma lease no dispositivo antes de comandos mutáveis em `SENSOR_DIRECT`.

Credenciais Wi-Fi e PSK não existem mais no código. O driver recebe SSID/senha em runtime, usa DHCP e expõe uma API que futuramente será chamada pelo provisionamento BLE. O comando serial `NET WIFI PROVISION` existe apenas para bancada/manutenção.

A descoberta UDP antiga ganhou jitter de 1,5 a 3,5 s. Esse mecanismo será substituído pelo beacon BLE, mas a política de jitter permanecerá útil para evitar sincronização de múltiplos sensores.

A TUI não aceita mais OTP por prefixo/tamanho como se fosse validação Yubico. Até a integração FIDO2/OTP real, o modo `otp` falha fechado. `security.json` exige `0600` e não degrada silenciosamente em caso de erro.

O cliente serial captura especificamente erros de transporte no loop de leitura e limita rajadas de escrita com intervalo mínimo curto.

Foi adicionada CI para testes da TUI e builds do Pico e do nó ESP32/PlatformIO.

## Deliberadamente não declarado como concluído

HMAC/FIDO2 real, persistência em flash e descoberta BLE distribuída ainda não são considerados concluídos nesta baseline. A mutação pelo protocolo UDP legado permanece bloqueada por padrão exatamente porque ainda não existe MAC criptográfico de payload.

A próxima etapa de implementação é o beacon BLE no Pico e o scanner BLE nos nós CAN, seguido pelo reporte de candidatos/RSSI via CAN e pela seleção de associação na TUI.

## Confirmação de comandos CAN

Os comandos direcionados ao sensor local de um módulo CAN (`22 10 ID ação`) passaram a gerar uma confirmação explícita no barramento. O nó alvo transmite `STATUS_OPCODE/0x52` com o subcomando original, a ação e resultado `APPLIED` ou `REJECTED`. O Probe 00 converte essa confirmação para `CMD_ACK` serial e a TUI a registra como `CommandAck`. Isso elimina o comportamento puramente fire-and-forget para essas ações sem alterar o mecanismo de eleição legado.

## Limite desta etapa

A origem de frames administrativos CAN ainda não possui MAC/autenticação criptográfica. O ACK confirma execução, não identidade do emissor. A correção completa exige um novo formato de controle com origem, contador monotônico e MAC, que será tratado junto da evolução do protocolo CAN/CAN FD.

## Rastreabilidade de desafios

O histórico consolidado dos problemas observados, das correções e dos itens ainda abertos está em `Documentacao/development/challenges-and-resolutions.md`.
