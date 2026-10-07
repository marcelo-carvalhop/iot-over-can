# Módulo CAN

Cada Node CAN usa ESP32 + MCP2515 e participa da lógica distribuída do barramento. O firmware é compilado por PlatformIO em `Codigo/node-can/`.

O ID não é editado no código-fonte. Ele é fornecido ao processo de build ou upload:

```bash
./Codigo/scripts/build_esp32_can_node.sh 1
./Codigo/scripts/upload_esp32_can_node.sh 1 /dev/ttyUSB0
```

O papel `LEADER` ou `FOLLOWER` é dinâmico e não define as capacidades do módulo. Capacidades como sensor local e scan BLE pertencem ao hardware/firmware do Node e permanecem disponíveis independentemente de seu papel distribuído.

A função local atual é `DEMO_BYTE`, com valor `0xAA`. Ela valida o transporte de um dado produzido pelo próprio Node. Futuras funções locais podem representar sensores ou atuadores reais e deverão declarar sua interface específica na TUI.

Nodes funcionais inicializam o scanner BLE passivo. O Node 0 é reservado à Probe 00 e não executa scan BLE.

## Associação de sensores wireless

Cada Node mantém uma tabela limitada de vínculos wireless. Um pedido de associação recebido pela Probe 00 contém o Node alvo e o UUID64 do sensor. O Node só aceita o vínculo quando o UUID foi observado recentemente pelo scanner BLE. O menor `child_id` livre é alocado e forma o identificador lógico `parent.child`.

O vínculo não depende do papel de líder ou seguidor. Após a confirmação, novos advertisements do mesmo UUID atualizam o lease de presença. O estado passa a `STALE` após 15 s sem advertisement e a `LOST` após 45 s desde a última observação. O reaparecimento do UUID recupera `ONLINE` sem recriar o vínculo. O scanner desabilita filtro de duplicatas e possui watchdog para reiniciar a varredura quando necessário.

`WIRELESS UNBIND` remove explicitamente o vínculo. O Node publica `UNBOUND`; somente essa confirmação autoriza a TUI a remover o filho lógico. Se o sensor continuar anunciando, volta a ser candidato disponível para nova associação.

Na TUI, `F7`/`w` centraliza candidatos, observações por RSSI, associações e desassociações. O melhor RSSI é recomendação, não decisão autônoma.

A associação ainda não é persistida em memória não volátil; uma reinicialização do Node remove a tabela local de vínculos. A persistência será adicionada antes de considerar o mecanismo de associação completo para implantação permanente.
