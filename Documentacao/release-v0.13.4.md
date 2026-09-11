# Release v0.13.4 — robustez serial e redução de ruído da TUI

Esta revisão consolida a descoberta BLE distribuída já validada em bancada e prepara a interface para a etapa de associação. Não altera o wire format BLE/CAN nem exige regravação dos firmwares.

Principais mudanças:

- handshake serial da TUI serializado e iniciado somente após estabilização da porta;
- descarte de bytes residuais após abertura e remoção de NULs do fluxo textual;
- `STATUS TX` e `CONTROLE RX` rotineiros deixam de aparecer como `GW_UNPARSED`;
- mensagens `UNPARSED` de nível DEBUG continuam disponíveis no JSONL para diagnóstico, mas não são renderizadas no EventLog operacional;
- valores periódicos do sensor local deixam de gerar uma linha por rodada;
- novo painel direito `TELEMETRIA DOS NÓS` mostra valor local, rodada, idade da amostra, estado BLE, quantidade de candidatos e melhor RSSI;
- atualizações sucessivas de RSSI alimentam o estado/painel, enquanto o EventLog registra somente a primeira descoberta de cada UUID por nó;
- a capacidade `BLE_SCAN` coloca o nó funcional em `SCANNING` mesmo antes do primeiro candidato;
- Probe 00 continua fora da topologia funcional e com BLE desativado.

A descoberta BLE completa foi observada em bancada com o mesmo sensor `0xE6616408432B6F39` reportado pelos quatro nós funcionais através do CAN clássico/MCP2515 e recebido pela TUI.
