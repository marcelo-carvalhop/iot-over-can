# Correção: controle de stream serial

Problema observado: com `TELEMETRY ON`, o terminal recebia linhas contínuas e ficava difícil digitar `TELEMETRY OFF`.

Mudanças:

- `TELEMETRY ON` agora tem rate limit, padrão 1000 ms.
- Novo alias curto: `TEL OFF`.
- Novos comandos:
  - `TELEMETRY FAST` -> 250 ms
  - `TELEMETRY SLOW` -> 1000 ms
  - `TELEMETRY PERIOD <ms>` -> 100 a 10000 ms
- Atalhos de emergência:
  - `!`
  - `Ctrl+C`
- O prompt não é reimpresso após toda linha de telemetria contínua.
