# Registro de validação em bancada física

Cada entrada registra uma sessão de testes com o hardware real: o que foi montado, o que funcionou, o que falhou e o que ficou em aberto. Os resultados da bancada virtual (`Codigo/node-can/test/host/`) não entram aqui.

## 08/10/2026 — primeira sessão do plano de dados wireless

### Montagem

| Item | Detalhe |
|---|---|
| Barramento | Probe 00 e quatro Nodes CAN (ESP32 + MCP2515), CAN clássico a 500 kbit/s |
| Sensor | um Raspberry Pi Pico W com MPU6050 |
| Firmware | branch `feat/network-metrics-v0.18`, versão 0.18.0, gravado pelos scripts de `Codigo/scripts/` |
| Credenciais | `.env.local` com chave mestra; sensor compilado no modo de bancada (`build_pico.sh` sem `--sensor`, `LINK_KEYS=MASTER`) |
| TUI | conectada à Probe 00 |

### O que funcionou

| Função | Resultado |
|---|---|
| Descoberta BLE do sensor e associação pela TUI | funcionou |
| Oferta autenticada do Node recebida e conferida pelo sensor | funcionou (`AUTH_REJECT=0`) |
| Reassociação automática | o sensor foi associado ao Node 4 com política automática; sem sessão com ele, o vínculo passou ao Node 2 sem intervenção do operador |
| Sessão autenticada sensor ↔ Node 2 | funcionou |
| Telemetria pelo barramento CAN até a TUI | funcionou |
| Espectro (FFT) sob pedido | funcionou |

### O que falhou ou ficou em aberto

1. **Sessão com o Node 4.** O sensor recebeu e conferiu a oferta do Node 4 e ligou o Wi-Fi para entrar no ponto de acesso dele (`STATE=DISCOVERY WIFI=ON PARENT_NODE=4`), mas a autenticação não terminou. Causa ainda não identificada. Suspeitas: alimentação dessa placa (pico de corrente do Wi-Fi), interferência (o Node 4 usa o canal 6, o mesmo do Node 1) ou defeito de firmware. O Node 4 era o líder, mas o firmware não impede o líder de atender sensores, e a bancada virtual exercita esse caso. Próximo passo: repetir a associação ao Node 4 com as mensagens de diagnóstico (`[WLINK]` no Node, `[NET]` no sensor).
2. **Alimentação.** Um power bank de 2000 mAh não manteve os ESP32 ligados (CH-053).
3. **Credenciais de versão anterior.** O `.env.local` existente era de uma versão anterior e não tinha a chave mestra. Nodes e sensor foram compilados sem chave, e os scripts apenas avisaram (CH-056). Corrigido com `provision_sensor_security.sh --add-link-key` e nova gravação.
4. **Exibição.** A TUI mostrava "ida e volta de 0,0 ms" para o enlace liberado pelo Node 4 (CH-054). Corrigido no firmware.

### Ainda não validado em bancada física

Tempos de reassociação e de sessão (nenhum foi medido nesta sessão), tela de métricas e análise de tempo de resposta, ensaios de falha, vigilância do líder pelos seguidores, configuração remota do sensor e todos os casos com mais de um sensor.
