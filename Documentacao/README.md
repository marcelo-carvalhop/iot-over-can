# Documentação do iot-over-can

A documentação oficial é organizada por assunto, sem cópias paralelas ou release notes que repitam o estado do projeto.

| Documento | Conteúdo |
|---|---|
| [`descrição-do-projeto.md`](descrição-do-projeto.md) | finalidade, escopo e conceitos |
| [`estrutura-do-projeto.md`](estrutura-do-projeto.md) | organização do repositório |
| [`requisitos.md`](requisitos.md) | requisitos funcionais, de interface, software e segurança |
| [`build_e_teste.md`](build_e_teste.md) | instalação, execução, build, upload e validação |
| [`modelo_de_seguranca.md`](modelo_de_seguranca.md) | autorização, arquivos locais, provisionamento e renovação |
| [`desafios_e_resolucoes.md`](desafios_e_resolucoes.md) | problemas técnicos com impacto arquitetural e soluções |
| [`arquitetura/arquitetura.md`](arquitetura/arquitetura.md) | elementos físicos, domínios, associação e camadas de software |
| [`arquitetura/plano-de-dados-wireless.md`](arquitetura/plano-de-dados-wireless.md) | enlace autenticado sensor ↔ Node, transporte no CAN, modelo de ameaças e roteiro de bancada |
| [`arquitetura/metricas-e-ensaios.md`](arquitetura/metricas-e-ensaios.md) | medição do barramento, análise de tempo de resposta, reassociação automática, vigilância do líder, ensaios de falha e bancada virtual |
| [`arquitetura/predisposicao-can-fd.md`](arquitetura/predisposicao-can-fd.md) | o que está preparado para CAN FD, o que a migração exigiria e a recomendação atual |
| [`protocolo/protocolo.md`](protocolo/protocolo.md) | CAN, Probe 00, BLE, associação, sensor, DTC e fragmentação |
| [`interface/tui.md`](interface/tui.md) | especificação completa da TUI, telas, comandos, wireless, layout e cores |
| [`modulo-can/modulo-can.md`](modulo-can/modulo-can.md) | firmware e comportamento dos Nodes CAN |
| [`sensor-wireless/sensor-wireless.md`](sensor-wireless/sensor-wireless.md) | firmware, aquisição e comunicação do Pico W |

Capturas de referência da interface ficam em [`interface/img/`](interface/img/). As imagens `antes_*` documentam somente a densidade/truncamento que as regras atuais de layout evitam.

Arquivos de release e `Codigo/node-can/legacy/` podem existir localmente, mas não fazem parte da documentação pública nem do código operacional versionado.
