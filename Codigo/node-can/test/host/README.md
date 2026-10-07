# Simulação do plano de dados wireless no computador

`sim_wireless_link.cpp` compila o arquivo real `Codigo/node-can/src/wireless_link.cpp`
junto com os substitutos de `stubs/` (Arduino, Wi-Fi, UDP, NimBLE e ACAN2515) e o
exercita nos dois papéis que ele tem no firmware:

```text
sensor simulado <-UDP-> wireless_link.cpp como Node 1 <-CAN-> wireless_link.cpp como Probe 00
```

O sensor simulado usa a biblioteca `Codigo/common/ioc_link` e as estruturas de
`Codigo/node-wifi/edge_protocol_definitions.h`, como o firmware do Pico W.

O que a simulação verifica:

- ponto de acesso e oferta BLE gerados na associação;
- recusa explícita de comandos enquanto não há sessão;
- handshake, telemetria periódica, espectro de 64 e de 256 faixas;
- configuração aceita e configuração recusada pelo sensor;
- controle do encaminhamento, limpeza de DTC, DTC urgente;
- repetição de datagrama, datagrama adulterado, sensor com chave errada e
  HELLO repetido durante uma sessão;
- expiração da sessão, recuperação e desassociação.

A saída padrão são as linhas que a Probe 00 enviaria à TUI.
`Front/tests/test_wireless_data_plane_v017.py` entrega essas linhas ao
decodificador e ao domínio reais da TUI.

```bash
./Codigo/scripts/build_wireless_link_sim.sh /tmp/sim_wireless_link
/tmp/sim_wireless_link
```

Limites: os substitutos não reproduzem temporização de rádio, coexistência
BLE/Wi-Fi nem a API completa das bibliotecas. A compilação para o ESP32 é
verificada pelo PlatformIO (integração contínua) e o comportamento em rádio,
em bancada. O diretório não faz parte do firmware: o PlatformIO só compila
`src/`.
