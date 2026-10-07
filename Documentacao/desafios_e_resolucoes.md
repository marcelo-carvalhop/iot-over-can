# Desafios e resoluções

Este documento registra defeitos técnicos que ajudam a compreender decisões atuais do projeto. O objetivo não é manter histórico de versões, mas registrar causa e solução de problemas que podem reaparecer.

| ID | Problema | Causa | Solução aplicada | Estado |
|---|---|---|---|---|
| CH-001 | DTCs conhecidos apareciam como desconhecidos | catálogo incompleto na TUI | catálogo explícito e fallback por categoria | Resolvido |
| CH-002 | bateria não instrumentada parecia 100% | sentinelas interpretadas como valor real | `255` e `65535` tratados como N/A | Resolvido |
| CH-003 | TUI dependia de porta serial no boot | inicialização acoplada ao transporte | inicialização offline e conexão posterior | Resolvido |
| CH-004 | IDs de Nodes podiam ser duplicados por build incorreto | valor de ID dependia de edição manual | ID explícito por script/`IOT_NODE_ID` | Resolvido |
| CH-005 | comandos mutáveis dependiam apenas da TUI | firmware aceitava ações locais sem sessão | `AUTH UNLOCK` e lease de administração no sensor | Resolvido |
| CH-006 | segredos estavam sujeitos a configuração inconsistente | múltiplas fontes e ausência de validação | `.env.local`, `security.json` 0600 e script de provisionamento | Resolvido |
| CH-007 | build do Pico W falhava ao ligar BTstack | configuração de hexdump ausente | `ENABLE_PRINTF_HEXDUMP` no `btstack_config.h` | Resolvido |
| CH-008 | Probe não respondia à autodetecção textual | parser aceitava apenas comandos CAN hexadecimais | `PROBE_VERSION` e `PROBE_STATUS` | Resolvido |
| CH-009 | EventLog era poluído por manutenção e telemetria | estado contínuo tratado como evento | classificação de manutenção e painel dedicado de Nodes | Resolvido |
| CH-010 | abertura serial podia produzir fragmentos e bytes NUL | dados residuais e handshake rápido demais | limpeza do buffer, atraso de estabilização e sanitização de entrada | Mitigado |
| CH-011 | associação wireless precisava preservar a escolha explícita do operador | um mesmo UUID pode ser visto por vários Nodes com RSSI diferente | vínculo lógico comandado pela TUI, confirmação pelo CAN e lease de presença por BLE; persistência permanece pendente | Parcial |
| CH-012 | CAN ainda usa MCP2515 clássico | hardware CAN FD ainda não integrado | migrar após estabilização do protocolo funcional | Aberto |
| CH-013 | filho wireless aparecia `STALE/LOST` apesar de o Node continuar recebendo advertisements | TUI e Node CAN calculavam liveness de forma independente, e o lease BLE de 6,5 s era agressivo | Node CAN tornou-se autoridade de liveness, lease ajustado para 15/45 s, filtro de duplicatas desabilitado e watchdog de scan | Resolvido |
| CH-014 | TUI acumulava informação simultânea e dependia de atalhos/comandos pouco visíveis | topologia, telemetria, configuração, eventos e comandos disputavam a mesma tela; ações equivalentes existiam em vários caminhos | telas cheias por assunto, barra contextual, catálogo único de comandos, mensagens opcionais, layout responsivo e gerenciamento wireless visível em `F7/w` | Resolvido |
| CH-015 | ações de telemetria/configuração pareciam disponíveis para filho wireless associado sem existir canal Pico W ↔ Node | associação lógica foi confundida com plano de dados | a TUI distingue vínculo de plano de dados, mostra a limitação e oculta/bloqueia ações sem caminho de entrega real | Resolvido |
| CH-016 | lista `wireless-available` excedia a largura em 48, 60 e 100 colunas | a mensagem do estado vazio `Nenhum sensor sem fio disponível foi observado nos últimos 15 s.` possui exatamente 64 células; as primeiras correções atuaram nos prompts de candidatos e não atingiram esse item `__empty__` | estado vazio dividido em duas linhas curtas, mantendo a informação dos 15 s; candidatos e vínculos continuam compostos por linhas semânticas curtas | Resolvido |

Novos problemas devem ser adicionados apenas quando houver impacto arquitetural, de protocolo, segurança, build ou operação. Logs transitórios de bancada não precisam gerar uma entrada permanente.
