# Auditoria integrada de robustez — outubro de 2026

## Escopo, referência e limites

Revisão da \`main\` após integração do USB-HIL e do lançador do Studio. O inventário GitHub contém 369 entradas na árvore de código e documentação; a revisão concentrou-se nos caminhos operacionais do Pico acústico, Node CAN, Probe, bancada virtual, Studio, TUI, métricas, histórico e transporte de espectros. Foram inspecionados os principais pontos de concorrência e buffers, além de testes existentes e textos das telas. **Não se trata de prova formal de ausência de bugs**, nem substitui ensaios no hardware, análise dinâmica com sanitizers, revisão sistemática de todas as linhas ou fuzzing prolongado.

A branch \`audit/integrated-robustness-2026-10\` isola os ajustes para revisão em pull request. Não há alterações na semântica do DSP, criptografia, política de associação ou transporte CAN real.

## Falhas identificadas e corrigidas

| Área | Condição anterior | Tratamento |
|---|---|---|
| Ponte pseudo-serial | \`os.write\` em FD não bloqueante podia escrever menos bytes que o solicitado sem lançar exceção; o restante do frame era perdido | Laço de escrita completa com \`select\` e prazo finito; descarte explícito/contabilizado se a TUI não acompanhar |
| Histórico | \`flush\` retirava e limpava a fila enquanto callbacks podiam adicionar eventos em paralelo | Lock próprio da fila; retirada atômica do lote, reintrodução ordenada sob falha de SQL e limite de capacidade |
| USB-HIL | Existindo linhas completas em \`pending\`, ainda se executava \`readline\` potencialmente bloqueante | Drenagem do buffer antes de nova leitura física; preserva mensagens parciais e limite de 16 KiB |
| Lançador | Duas inicializações simultâneas podiam passar juntas pela verificação de PID | \`flock(LOCK_NB)\` em arquivo exclusivo da instalação, serializando a partida |
| CPU acústico | Percentual era convertido para \`uint8_t\` antes de limitar a 100%, permitindo wrap de leituras anômalas | Cálculo em 64 bits, clamp e conversão apenas no final |
| Histórico de CAN | Média de ocupação tratava igualmente janelas de duração variável | Média ponderada pela duração \`window_ms\`; preserva soma de quadros e valor máximo de pico |
| Reagrupador CAN | Fragmentos, contagens e transferências simultâneas sem teto; CRC diferente por fragmento ignorado | Limites de memória explícitos, rejeição de metadados conflitantes, manutenção de até 64 KiB por transferência e CRC final |
| Terminologia de interface | Rodapé do Studio afirmava que nenhuma leitura era física; instrução de captura redundante | Distingue tráfego e enlaces modelados de leitura acústica real via USB; revisão pontual dos rótulos |

## Regressões acrescentadas

\`Simulacao/tests/test_pty_partial_write.py\` reproduz escritas curtas, \`EAGAIN\` e PTY bloqueada. \`Simulacao/tests/test_usb_buffered_lines.py\` exige drenagem imediata das linhas completas. \`Simulacao/tests/test_lab_launcher.py\` cobre duas partidas concorrentes. \`Front/tests/test_history_thread_safety.py\` coordena eventos de múltiplas threads durante escrita SQL normal e falha recuperável, verifica fechamento idempotente e média ponderada de CAN. \`Front/tests/test_fragment_bounds.py\` cobre integridade de FFT acima de um quadro CAN, limite de memória, excesso de transferências e conflito de CRC.

Esses testes foram **adicionados**, mas seu resultado precisa ser confirmado pelo GitHub Actions ou no checkout local após a revisão. Não confundir existência de um teste com resultado aprovado.

## Validação necessária antes do merge

Na raiz do projeto:

\`\`\`bash
python3 -m unittest discover -s Simulacao/tests -v
bash Simulacao/verify.sh
./Codigo/scripts/build_network_sim.sh /tmp/ioc_sim_network
./Codigo/scripts/test_native_firmware.sh
Front/.venv/bin/python -m pytest Front/tests -v
\`\`\`

Se houver SDK/compilador disponível, também compilar o firmware acústico no alvo Pico W e a plataforma ESP32, conforme os scripts da pasta \`Codigo/scripts\`. Inspecionar GitHub Actions (jobs da TUI, bancada virtual e builds); nenhum resultado pode ser presumido enquanto estiver \`queued\` ou \`in_progress\`.

No laboratório físico: manter telemetria USB-HIL por pelo menos 10 minutos, solicitar várias FFTs com telemetria ativa, observar RTT, descartes de PTY, intercorrências, valores de CPU, variação de carga CAN e reconstrução das capturas no histórico. Repetir sob congestionamento CAN e desconexão/reconexão do Pico. Verificar se nenhuma FFT é inventada e se a perda de pacotes aparece em log.

## Riscos ainda não eliminados

A escrita para a PTY continua limitada por tempo; caso a TUI pare de consumir, o simulador poderá descartar mensagens após o prazo. Isso é preferível a travar indefinidamente o simulador, mas exige visibilidade dos descartes e teste de saturação. A TUI utiliza filas de eventos e amostras, e a presença de locks locais não prova que todos os caminhos concorrentes sejam isentos de data races.

As estimativas de CAN são baseadas em quadros observados/modelados e não representam necessariamente carga elétrica física ou retransmissões. A média histórica agora é temporalmente ponderada, mas estudos com tráfego CAN real precisam comparar taxa, jitter, perdas e contadores de erro medidos por hardware.

Os buffers do firmware Pico e Node CAN (incluindo filas multicore, retransmissões e reassembly no microcontrolador) precisam de validação com hardware sob carga, inclusive após muitas horas de operação. A auditoria estática não pode certificar a ausência de *deadlocks*, corrupção de memória ou overflow em todas as combinações de eventos.

## Simplificação e remoção de código

Não remover sem rastrear dependências os instaladores/patches históricos em \`Simulacao/patch_*.py\`, arquivos de resultados já versionados, gravações de replay ou componentes legados da TUI. Alguns servem a testes de regressão ou à reprodução de cenários; sua remoção indiscriminada faria perder cobertura e documentação experimental. Em uma etapa específica de limpeza, identificar referências pelo repositório inteiro, deprecar com aviso, retirar somente o que não tem consumidores comprovados e executar CI com comparação de cobertura. Não misturar exclusões em massa com a atual correção de robustez.
