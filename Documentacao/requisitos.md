# Requisitos

## Requisitos funcionais

| Requisito | Estado |
|---|---|
| Operação da TUI sem hardware conectado | Implementado |
| Seleção e troca de porta serial | Implementado |
| Instrumentação da rede pela Probe 00 | Implementado |
| Eleição e supervisão dos Nodes CAN | Implementado |
| Consulta e administração direcionada de Node CAN | Implementado |
| Função local de demonstração `DEMO_BYTE` nos Nodes CAN | Implementado |
| Catálogo de DTCs e visualização de diagnóstico | Implementado |
| Telemetria e FFT do sensor de vibração por acesso USB direto | Implementado |
| Autorização de comandos mutáveis no sensor wireless | Implementado |
| Descoberta BLE do mesmo UUID por múltiplos Nodes CAN | Implementado |
| Transporte de UUID, perfil, protocolo e RSSI pelo CAN | Implementado |
| Comparação das observações de RSSI | Implementado |
| Associação explícita de candidato wireless a um Node CAN | Implementado |
| Criação do filho lógico `parent.child` após confirmação do Node | Implementado |
| Desassociação explícita com confirmação `UNBOUND` | Implementado |
| Liveness do vínculo autoritativo no Node CAN | Implementado |
| Oferta de vínculo e sessão autenticada entre sensor e Node | Implementado; validação em bancada pendente |
| Telemetria e espectro wireless através do Node CAN associado | Implementado; validação em bancada pendente |
| Comandos/configuração do Pico W através do Node associado | Implementado; validação em bancada pendente |
| Medição de ocupação do barramento e de período por identificador pela Probe 00 | Implementado; validação em bancada pendente |
| Tempo de ida e volta do enlace sem fio e tempo de resposta de comandos | Implementado; validação em bancada pendente |
| Análise de tempo de resposta e estimativa de capacidade a partir do tráfego observado | Implementado |
| Reassociação automática de sensores, autorizada por sensor | Implementado; validação em bancada pendente |
| Recuperação da falta do líder sem depender da Probe 00 | Implementado; validação em bancada pendente |
| Ensaios de injeção de falhas com medição do resultado | Implementado; validação em bancada pendente |
| Verificação do firmware do Node CAN em bancada virtual | Implementado |
| Persistência e recuperação da associação | Pendente |
| Monitoramento da alimentação dos Nodes, com aviso na TUI | Pendente; proposta em `modulo-can/modulo-can.md` |
| CAN FD no hardware final | Pendente; predisposição em biblioteca |

## Requisitos da interface

A TUI deve ser operável sem depender de comandos ocultos ou de memorização de atalhos especiais. A tela inicial deve indicar condição geral, itens que exigem atenção, equipamentos, rede e conexão. Cada equipamento deve possuir uma tela própria, e comandos operacionais devem estar centralizados em uma única tela com linguagem de operador, parâmetros guiados e confirmação para ações críticas.

A navegação normal deve exigir apenas setas, `Enter`, `Esc` e a barra de teclas visível. Ações globais devem possuir tecla de função e letra equivalente. Gerenciamento wireless deve ter uma tela de primeiro nível visível por `F7`/`w`; comandos internos permanecem somente como recurso avançado.

As métricas da rede e os ensaios de falha devem ter telas próprias (`F8`/`b` e `F9`/`e`). O resultado de um ensaio deve ser medido pela TUI a partir das mensagens da rede, sem anotação manual, e poder ser exportado. Toda medida deve indicar de onde vem; uma estimativa (análise de tempo de resposta, capacidade) deve vir acompanhada de sua ressalva.

A interface deve funcionar em terminais estreitos sem depender de rolagem horizontal. Texto essencial não pode ser truncado. Em largura inferior a 100 colunas, as seções passam para uma coluna; abaixo de 60 colunas, bordas laterais são removidas para recuperar espaço útil. A altura curta também reduz espaçamentos e a faixa de estado.

Eventos e estado contínuo devem permanecer separados. RSSI, função local `0xAA` e valores periódicos atualizam estado; não devem gerar uma nova mensagem operacional a cada amostra. O histórico JSONL continua registrando dados técnicos necessários ao diagnóstico.

Cores devem representar significado operacional: superfícies neutras no estado normal, azul para foco/interação, âmbar para atenção e vermelho para crítico. Nenhum estado pode depender somente de cor; símbolo e palavra devem acompanhá-lo.

Um sensor wireless associado sem sessão autenticada com o módulo não deve expor botões de telemetria, FFT, configuração ou comandos que não possam ser entregues ao Pico W. A tela deve distinguir **vínculo lógico/liveness** de **plano de dados** e informar o estado deste último.

Uma função local implementada em um Node CAN deve possuir interface própria de leitura e configuração quando necessário e nunca deve ser representada como sensor wireless associado.

## Requisitos de software da TUI

Python 3.11 ou superior. `Front/pyproject.toml` é a fonte única de dependências e fixa o ambiente validado:

```text
textual == 8.2.8
rich == 15.0.0
pyserial == 3.5
pytest == 9.1.1          # desenvolvimento
pytest-asyncio == 1.4.0  # desenvolvimento
ruff == 0.16.7           # desenvolvimento
```

Não existem `requirements.txt` paralelos.

O firmware ESP32 usa PlatformIO com framework Arduino, ACAN2515, NimBLE-Arduino e a biblioteca local `Codigo/common/ioc_link`. O firmware do Pico W usa Pico SDK, BTstack/CYW43, lwIP e toolchain ARM compatível com o SDK configurado.

## Requisitos de segurança

Segredos não podem ser versionados. `security.json` e `.env.local` devem usar permissão `0600` em POSIX. O modo OTP permanece fail-closed enquanto não houver verificação criptográfica real.

A TUI deve tratar como mutáveis, entre outros, os comandos administrativos CAN `22 00`, `22 10`, `22 30`, comandos `CMD ...`, `WIRELESS BIND/UNBIND/POLICY` e `FAULT ...`. A ausência de autorização deve bloquear o envio, não apenas ocultar a ação.

A reassociação automática de um sensor só pode ocorrer quando o operador a autorizou para aquele sensor. O tratamento dos comandos de ensaio nos Nodes deve poder ser removido na compilação.

O firmware do sensor direto mantém sua própria autorização. A TUI não é a única barreira de segurança.

O enlace entre sensor e Node deve autenticar os dois lados antes de transportar dados, rejeitar datagramas adulterados ou repetidos e permanecer desligado quando não houver chave provisionada. Não pode existir modo de compatibilidade sem autenticação.
