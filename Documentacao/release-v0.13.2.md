# Release v0.13.2 — correção do handshake da Probe 00

A conexão serial da TUI estava sendo marcada como aberta, mas a Probe 00 não era
identificada. A TUI enviava `GW_VERSION` e `GW_STATUS` durante a sondagem inicial,
enquanto o firmware CAN tratava qualquer linha serial como quatro bytes hexadecimais.
O resultado era `[CTRL] Formato invalido` e, em seguida, `GW_UNPARSED` na TUI.

A v0.13.2 introduz um protocolo textual mínimo de introspecção, que não injeta
qualquer frame no barramento:

- `PROBE_VERSION` → versão do firmware/protocolo da Probe;
- `PROBE_STATUS` → estado da instrumentação e do CAN;
- aliases `GW_VERSION`, `GW_STATUS`, `VERSION` e `STATUS` permanecem aceitos na
  Probe 00 para compatibilidade e detecção automática;
- a TUI usa `PROBE_*` como nomes canônicos;
- a Probe 00 deixa de anunciar `BLE_SCAN` em sua linha de capacidades.

A lógica distribuída dos Nodes 1..N e o protocolo CAN existente não foram alterados.
