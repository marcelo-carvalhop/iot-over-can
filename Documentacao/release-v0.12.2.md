# Release v0.12.2 — correção do build DHCP do node-wifi

Esta release corrige uma regressão de linkedição introduzida durante a migração
do sensor wireless de endereço IPv4 estático para DHCP.

O `edge_network_driver.c` já chamava `dhcp_start()` e `dhcp_stop()`, porém
`Codigo/node-wifi/lwipopts.h` mantinha `LWIP_DHCP` desabilitado. Como consequência,
o cabeçalho podia ser incluído, mas as funções não eram compiladas na biblioteca
lwIP, resultando em `undefined reference` durante a geração de
`edge_node_firmware.elf`.

Correções:

- `LWIP_DHCP` alterado de `0` para `1`;
- teste de regressão garante que código e configuração lwIP permaneçam coerentes;
- `scripts/build_pico.sh` valida explicitamente a geração de
  `Codigo/node-wifi/build/edge_node_firmware.uf2`;
- desafio registrado como `CH-022`.

Não houve alteração do protocolo wireless, da arquitetura distribuída ou da TUI.
