# Histórico de sessões — etapa 1

A TUI grava **eventos relevantes e indicadores CAN agregados**, mas **não grava
telemetria contínua em SQLite**. Os sensores e Nodes permanecem independentes da
estação de supervisão: a ausência do computador não impede eleições e failover.

## Persistência

O arquivo `logs/historico.sqlite3` contém as tabelas `sessions`, `events` e
`bus_samples`. As chaves estrangeiras e índices por sessão/tempo permitem
reconstrução de sessões anteriores. Eventos contêm categoria, severidade,
instante de recepção, Node e sensor lógico quando informados. Quando disponível,
o último relatório CAN observado fica também no JSON contextual do evento.

As estatísticas de barramento são agregadas por janela de **30 segundos**;
armazenam carga média, pico máximo, quadros recebidos e estado de erros.
Elas **não** representam todos os quadros individuais. Ausência de amostra não
representa carga zero. Não existe mecanismo de sincronização dos relógios dos
microcontroladores nesta etapa; os horários representam observação pela TUI.

O `HistoryService` recebe eventos do `EventBus`, deduplica estados repetidos e
utiliza uma fila limitada com gravação SQL em lotes. Em uma queda do processo,
eventos ainda não descarregados podem se perder. O transbordo da fila é
registrado explicitamente. Falhas do banco não bloqueiam o plano de controle.

## Exploração

A tela **Histórico** abre com `F11` ou `h`. Selecione uma sessão na lista
superior. A linha do tempo exibe cinco categorias (`NETWORK`, `CAN`, `WIRELESS`,
`SENSOR`, `SYSTEM`); a região inferior apresenta os eventos da janela e os
detalhes selecionados. `←` e `→` escolhem ocorrências, `PageUp/PageDown`
deslocam a janela, `+` e `-` alteram a escala, e `u` atualiza as sessões.

A janela solicita no máximo 500 eventos; quando houver mais, a tela indica
que o resultado é parcial. A redução de zoom permite consultar intervalos
menores. As referências a arquivos de captura e a telemetria contextual serão
implementadas apenas na etapa 2, preservando essa distinção visual.

## Verificação

```bash
cd Front
python -m pytest -q tests/test_history_v1.py tests/test_history_screen_v1.py
python -m pytest -q tests/test_livro.py
```

O script `Codigo/scripts/extrair_livro.py` codifica espaços em destinos
Markdown relativos com `%20`. A validação decodifica os caminhos antes de
consultar o sistema de arquivos. Isso evita falhas de teste em checkouts cujos
diretórios têm espaços.
