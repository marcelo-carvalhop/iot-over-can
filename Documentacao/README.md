# Documentação

## Arquivos principais

- `project-description.md`: descrição conceitual do projeto.

- `build-and-test.md`: build do firmware e execução da TUI.
- `security-model.md`: modelo de segurança operacional com YubiKey.
- `integration-changelog.md`: mudanças de integração realizadas.
- `project-structure.md`: estrutura original do pacote integrado.
- `requirements-coverage.md`: cobertura dos requisitos da TUI.

## Protocolo

- `protocol/serial-baseline-protocol.md`: protocolo ASCII atual do sensor.
- `protocol/gateway-can-recommendations.md`: recomendações CAN/CAN FD.
- `protocol/gateway-protocol-reference.md`: referência de gateway usada pela TUI.

## Arquitetura

- `architecture/tui-integration-notes.md`: notas de integração da TUI.

- `protocol/dtc-catalog.md`: catálogo objetivo de DTCs e política de fallback.

- `architecture/tui-network-console.md`: decisão de interface para console genérico da rede CAN.

## Registro de engenharia

- `development/challenges-and-resolutions.md`: tabela histórica de desafios observados, causas, soluções implementadas, estado e baseline correspondente. Também mantém os desafios deliberadamente abertos para as próximas etapas.

## Releases

- `release-v0.12.0.md`: escopo consolidado, validações executadas e limites da baseline v0.12.0.

## Organização acadêmica

A documentação está centralizada nesta pasta. O código executável está separado em:

- `../Front/`: TUI.
- `../Codigo/node-can/`: firmware dos módulos CAN.
- `../Codigo/node-wifi/`: firmware do sensor IoT wireless.

A descrição completa da organização está em `project-structure.md`.
