# Descoberta wireless distribuída — v0.13.0

Esta versão implementa o primeiro caminho wireless real do projeto: o Pico W anuncia
sua identidade por BLE, cada Node CAN funcional mede RSSI e reporta o candidato pelo
barramento CAN clássico, e a Probe 00 reagrupa a informação para a TUI.

```text
Pico W -- BLE --> Node 01/02/03/04 -- CAN clássico --> Probe 00 --> TUI
```

A Probe 00 não faz scan BLE e continua fora da lógica funcional. Como a bancada usa
MCP2515, o UUID64, perfil e RSSI são transportados em dois frames CAN de 8 bytes. A
TUI mantém candidatos separados dos sensores associados; portanto nenhum candidato
recebe `parent.child` nesta versão.

O advertisement BLE contém Manufacturer Specific Data com identificador de
desenvolvimento `0xFFFF`, magic `IC`, versão do formato, perfil, versão de protocolo
e UUID64. O Wi-Fi continua desligado até uma futura etapa de associação.

Ainda não implementado: seleção de Node na TUI, comando de associação, provisionamento
Wi-Fi pelo Node escolhido, sessão UDP operacional e atribuição de `child_id`.
