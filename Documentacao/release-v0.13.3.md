# Release v0.13.3 — observabilidade da Probe e BLE na TUI

Esta revisão altera apenas a TUI. Respostas reconhecidas `PROBE_VERSION` e `PROBE_STATUS` continuam atualizando o modelo interno da instrumentação e agora também são registradas no EventLog com origem `PROBE`. O comando de atualização de status da interface passa a usar `PROBE_STATUS` em vez do alias legado `GW_STATUS`.

Para apoiar a validação da descoberta wireless, linhas `WIRELESS_CANDIDATE` reconstruídas pela Probe continuam atualizando o estado interno do nó observador e agora também aparecem no EventLog com origem `BLE`.

Não há alteração no firmware da Probe, nos nós CAN, no protocolo de descoberta BLE ou no firmware do Pico W.
