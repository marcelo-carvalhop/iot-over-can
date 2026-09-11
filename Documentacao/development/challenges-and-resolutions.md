# Registro de desafios encontrados e resoluções

Este documento mantém o histórico técnico das dificuldades encontradas durante a evolução do `iot-over-can`. O objetivo é registrar não apenas o estado final do código, mas também o problema observado, sua causa e a decisão adotada. Isso facilita revisões futuras, evita regressões e fornece rastreabilidade para documentação acadêmica e experimental.

A coluna **Estado** usa três classificações: `RESOLVIDO`, quando a correção já faz parte da baseline; `MITIGADO`, quando a baseline elimina o risco imediato mas a solução definitiva depende de uma etapa futura; e `ABERTO`, quando o item foi deliberadamente deixado para a próxima fase.

## Desafios resolvidos ou mitigados

| ID | Desafio observado | Impacto / causa | Resolução adotada | Estado | Baseline |
|---|---|---|---|---|---|
| CH-001 | TUI não iniciava sem hardware serial conectado | O fluxo de boot condicionava a interface à seleção/abertura de uma porta | Inicialização `offline-first`; a TUI abre em `DISCONNECTED` e a conexão passa a ser uma ação posterior | RESOLVIDO | 0.10 |
| CH-002 | `NameError` no estado vazio da TUI | `sensor_count` era usado antes de ser calculado quando não havia sensor selecionado | Contagem calculada antes do ramo `sensor is None` e teste de regressão adicionado | RESOLVIDO | 0.10 |
| CH-003 | Tela de portas falhava com CP2102 | O HWID entre colchetes era interpretado como markup pelo Textual/Rich | `OptionList(markup=False)` para a lista de portas, preservando o HWID como texto literal | RESOLVIDO | 0.10 |
| CH-004 | Comandos globais CAN dependiam da seleção de um nó | Eleição e consulta de rede foram tratadas como ações contextuais de módulo | Tela de comandos globais, `:election`, `:canstatus` e acesso independente de nó selecionado | RESOLVIDO | 0.10 |
| CH-005 | Probe 00 aparecia como membro funcional da rede | O modelo inicial confundia instrumentação com nó da lógica distribuída | ID 0 removido da topologia funcional; Probe 00 passou a ser apenas instrumentação/observação | RESOLVIDO | 0.11 |
| CH-006 | Telemetria `0xAA` era confundida com sensor wireless | O dado legado do próprio ESP32 criava a impressão de um `child_id` remoto | `0xAA` passou a `LOCAL_SENSOR_DEMO_VALUE`, com `LOCAL_PROFILE=DEMO_BYTE` dentro do módulo CAN | RESOLVIDO | 0.11 |
| CH-007 | Clicar no nó não abria informações e DTC era orientado só a sensor remoto | Seleção interna não estava conectada a uma tela viva de módulo | `CanNodeDetailScreen` viva, DTC próprio de módulo, botões de ação e atualização periódica | RESOLVIDO | 0.11.1 |
| CH-008 | Telas de vibração apareciam mesmo sem sensor de vibração associado | A navegação era centrada no MPU6050, não em capacidades/perfis | UI orientada por `PROFILE_ID`; telemetria/FFT de vibração só aparecem para `PROFILE=VIBRATION` | RESOLVIDO | 0.11 |
| CH-009 | Validação de `sample_rate_hz` era diferente entre serial e rede | O caminho de rede podia aceitar valores que só seriam limitados em camada inferior | `config_validation.c/.h` centraliza limites e valida antes de qualquer cast/aplicação | RESOLVIDO | 0.12 |
| CH-010 | UUID do sensor era fixo | Vários sensores compilados com a mesma constante teriam identidade indistinguível | UUID64 derivado do identificador único da placa através de `pico_unique_id` | RESOLVIDO | 0.12 |
| CH-011 | SSID, senha e chave de vínculo estavam hardcoded | Segredos eram versionados e a configuração não escalava | Credenciais removidas do código; provisionamento local/runtime e `.env.local` ignorado pelo Git | RESOLVIDO | 0.12 |
| CH-012 | Sensor usava IP estático e não escalava para múltiplas unidades | Endereço `192.168.4.2` produziria colisões | Migração para DHCP no caminho Wi-Fi legado | RESOLVIDO | 0.12 |
| CH-013 | Beacons podiam se sincronizar após perda simultânea de vínculo | Intervalo fixo favorecia rajadas simultâneas (`thundering herd`) | Intervalo de descoberta com jitter aleatório de aproximadamente 1,5–3,5 s | RESOLVIDO | 0.12 |
| CH-014 | Segurança de comandos existia apenas na TUI | Qualquer terminal serial bruto podia executar comandos mutáveis no Pico | Lease administrativa no firmware com `AUTH UNLOCK`, `AUTH STATUS` e `AUTH LOCK` | RESOLVIDO | 0.12 |
| CH-015 | `RESET` era imediato | Uma única linha poderia reiniciar o dispositivo sem confirmação nem distinção operacional | Fluxo `RESET` → `RESET CONFIRM` com janela temporal de confirmação | RESOLVIDO | 0.12 |
| CH-016 | Modo OTP aceitava apenas formato/prefixo e podia degradar silenciosamente | Não havia validação criptográfica real; arquivo inseguro podia reduzir proteção | OTP passa a fail-closed; `security.json` exige proprietário correto e `0600`; erro vira `SEC=CONFIG_ERROR` | MITIGADO | 0.12 |
| CH-017 | Detecção de presença de YubiKey confiava em strings USB | Metadados descritivos podem ser falsificados por outro dispositivo | Presença restringida ao Vendor ID USB `0x1050`; descrições não são usadas como prova | MITIGADO | 0.12 |
| CH-018 | `serial_client` tratava erros de programação como desconexão | `except Exception` amplo mascarava defeitos internos | Tratamento direcionado a falhas de transporte e controle mínimo de rajadas de escrita | RESOLVIDO | 0.12 |
| CH-019 | DTC de segurança agrupava causas distintas | Autenticação, versão, replay e configuração inválida eram pouco distinguíveis | Catálogo e emissão separados para rejeição de autenticação, incompatibilidade, replay e configuração | RESOLVIDO | 0.12 |
| CH-020 | Comandos CAN administrativos eram `fire-and-forget` | Não havia confirmação de que a ação foi realmente aplicada no alvo | ACK explícito `APPLIED`/`REJECTED` para comandos direcionados ao módulo | RESOLVIDO | 0.12 |
| CH-021 | Modelo anterior pressupunha gateway wireless central | Não atendia ao requisito de nós CAN fisicamente distribuídos e sensor remoto sem acesso direto do operador | Arquitetura redefinida: todos os nós CAN elegíveis podem descobrir sensores; operador escolhe o melhor nó; Probe 00 permanece invisível | RESOLVIDO | 0.12 |
| CH-022 | Build do `node-wifi` falhava no linker em `dhcp_start`/`dhcp_stop` | `edge_network_driver.c` passou a usar DHCP, mas `lwipopts.h` ainda definia `LWIP_DHCP 0`, removendo a implementação DHCP do lwIP | `LWIP_DHCP` habilitado, teste de regressão adicionado e `build_pico.sh` passou a validar a existência do `.uf2` | RESOLVIDO | 0.12.2 |

## Desafios em aberto

| ID | Desafio pendente | Motivo de permanecer aberto | Próxima direção |
|---|---|---|---|
| OP-002 | Seleção e associação do sensor pela TUI | A descoberta BLE e o transporte de candidatos já foram validados; ainda falta o protocolo de vínculo | Tela de candidatos → escolha do nó → comando de associação → criação `parent.child` |
| OP-003 | Autenticação criptográfica do vínculo wireless | O caminho UDP legado está restrito/compatível, mas não há MAC/HMAC final do payload | Desafio-resposta com chave por dispositivo, contador monotônico e MAC |
| OP-004 | Autenticação de origem dos comandos CAN | ACK confirma execução, mas não prova identidade do emissor | Evoluir formato de controle com origem, contador e MAC compatível com o domínio CAN/CAN FD |
| OP-005 | FIDO2/Yubico OTP real | A validação antiga foi corretamente desativada, mas não substituída por verificador criptográfico | Implementar FIDO2 ou validação Yubico OTP real antes de reativar `otp` |
| OP-006 | Persistência da configuração do sensor | Configurações operacionais ainda são voláteis após reset | Armazenamento não volátil com versão, CRC e fallback para defaults |
| OP-007 | Comparação experimental POLLING × DRDY | POLLING é a baseline estável, mas DRDY é relevante para experimentos de temporização | Reexpor DRDY como modo experimental isolado, sem torná-lo requisito operacional |
| OP-008 | Build real das duas toolchains nesta entrega | O ambiente de empacotamento não contém Pico SDK completo nem PlatformIO | CI e validação no ambiente de desenvolvimento/runner com as toolchains instaladas |

## Regra de manutenção

Todo problema que alterar arquitetura, protocolo, segurança, comportamento observável da TUI ou procedimento de build deve receber um novo identificador `CH-xxx` ou `OP-xxx`. Quando um item aberto for concluído, ele deve ser movido para a tabela de desafios resolvidos, mantendo a versão em que a solução entrou na baseline.

## Organização acadêmica do repositório

| ID | Desafio | Impacto / causa | Solução adotada | Estado | Versão |
|---|---|---|---|---|---|
| ORG-001 | Estrutura do repositório divergente do padrão da disciplina | Código, TUI e documentação estavam distribuídos em `firmware/`, `software/tui/` e `docs/` | Migração para `Front/`, `Codigo/node-can/`, `Codigo/node-wifi/` e `Documentacao/`; scripts, CI, testes e referências internas foram atualizados | Resolvido | v0.12.1 |
| ORG-002 | Documentação espalhada entre código e interface | READMEs e changelogs existiam junto aos firmwares e à TUI | Toda documentação histórica foi copiada ou movida para `Documentacao/`, preservando apenas os arquivos locais necessários ao empacotamento/operação | Resolvido | v0.12.1 |

| ORG-003 | Testes ainda construíam caminhos com `firmware/...` e `software/tui/...` após a migração | A reorganização física alterou a profundidade e os caminhos absolutos calculados pelos testes | Testes atualizados para `Codigo/node-wifi`, `Codigo/node-can` e `Front`, mantendo a validação da baseline após a nova estrutura | Resolvido | v0.12.1 |

| CH-023 | UUID, perfil e RSSI não cabem em um frame CAN clássico | MCP2515 limita o payload a 8 bytes | Protocolo de descoberta dividido em duas mensagens com IDs derivados do Node reportador | RESOLVIDO | 0.13.0 |
| CH-024 | Probe 00 não deve virar gateway wireless | A descoberta poderia ser centralizada incorretamente no ponto USB | Scan BLE habilitado somente em `NODE_ID > 0`; Probe apenas observa/reagrupa | RESOLVIDO | 0.13.0 |
| CH-025 | Hardware real é Pico W/RP2040 | Scripts ainda usavam `pico2_w` | `pico_w` passou a ser padrão; `pico2_w` permanece opcional | RESOLVIDO | 0.13.0 |
| CH-026 | Build de Node CAN aceitava ID 4 implicitamente | O default silencioso podia gerar múltiplos módulos com a mesma identidade | Scripts de build/upload passaram a exigir e validar `NODE_ID` entre 0 e 31 | RESOLVIDO | 0.13.0 |

| CH-027 | Handshake serial da TUI gerava rajadas e resíduos após abertura da porta | Abertura USB podia resetar o ESP32 e a TUI enviava múltiplas sondagens antes da estabilização da serial | Adicionada janela de estabilização, descarte de entrada residual, remoção de NULs e handshake AUTO serializado a partir de um único `VERSION` inicial | RESOLVIDO | 0.13.4 |
| CH-028 | Mensagens normais de manutenção apareciam continuamente como `GW_UNPARSED` | Linhas como `[STATUS TX]` e `[GW] CONTROLE RX 23 50 ...` não eram classificadas pelo decoder textual | O decoder passou a consumir manutenção conhecida silenciosamente e a interpretar atualizações `CONTROLE RX`; `UNPARSED` DEBUG permanece persistido em arquivo, mas não polui o EventLog operacional | RESOLVIDO | 0.13.4 |
| CH-029 | Telemetria local `0xAA` e atualizações repetidas de RSSI poluíam o EventLog | Valores contínuos eram tratados como eventos discretos | Telemetria contínua foi movida para o painel direito `TELEMETRIA DOS NÓS`; o EventLog registra apenas a primeira descoberta BLE por nó/UUID e mudanças relevantes | RESOLVIDO | 0.13.4 |
| CH-030 | Estado BLE dos nós funcionais aparecia como `OFF/NOT_IMPLEMENTED` antes do primeiro candidato | A capacidade `BLE_SCAN` era conhecida, mas não era convertida em estado operacional na TUI | Nós que anunciam `BLE_SCAN` passam a `SCANNING`; Probe 00 permanece explicitamente com BLE desativado | RESOLVIDO | 0.13.4 |
| CH-031 | Descoberta BLE distribuída precisava ser validada no caminho completo | Era necessário comprovar `Pico W → ESP32 → CAN clássico → Probe 00 → TUI` com múltiplos observadores | Teste de bancada confirmou o mesmo UUID/perfil/protocolo reportado pelos Nodes 1–4 com RSSI independente e decodificação pela TUI | RESOLVIDO | 0.13.4 |
