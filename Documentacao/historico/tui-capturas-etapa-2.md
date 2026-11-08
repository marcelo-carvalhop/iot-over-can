# Capturas contextuais (etapa 2)

O histórico da TUI utiliza SQLite somente para **sessões, eventos, agregados de
rede e metadados de capturas**. Telemetria contextual e indicadores detalhados
são gravados como CSV fora do banco, em `logs/capturas/AAAA-MM-DD/<id>/`.

## Disparo e abrangência

Um DTC de severidade WARNING/CRITICAL aciona uma captura do sensor afetado,
incluindo seu enlace, estado de seu Node e indicadores CAN. Perda de sessão,
rejeição/perda de associação e falha de transferência desencadeiam captura do
Node, abrangendo todos os sensores associados. Failover, eleição de líder,
falhas CAN e saturação sustentada desencadeiam captura de rede, incluindo os
sensores observados pela TUI. Ensaios de injeção de falhas registram o Node
alvo ou a rede. Eventos de recuperação ocorridos durante a janela podem
compartilhar a mesma evidência.

Os intervalos padrão são **60 segundos anteriores** e **120 segundos posteriores**
ao gatilho, com limite de extensão de **10 minutos** por captura. A coleta
prévia é limitada ao que já chegou à TUI; não recompõe amostras perdidas nem
recupera sinais brutos que não foram encaminhados pelo Node CAN. Há no máximo
8 capturas simultâneas; a ausência de captura por limite é contada pelo serviço
em memória e não suprime o evento histórico.

## Persistência e consistência

Os dados recebidos são mantidos em buffers limitados e capturas ativas. Um
worker periódico grava CSVs em fragmentos imutáveis, usando arquivo temporário,
`fsync` e substituição atômica. O banco SQLite registra `captures`,
`capture_events` e `capture_files`; cada CSV é referenciado por caminho relativo
e checksum SHA-256. Consultas verificam tanto existência quanto integridade.
Falhas de escrita preservam amostras pendentes até o limite do buffer; não
bloqueiam o processamento do rádio/CAN.

As novas tabelas são criadas por migração não destrutiva ao reabrir um banco
da etapa 1. A tela Histórico utiliza os marcadores da faixa CAPTURA e apresenta
arquivos existentes, estado da coleta, linhas e exemplos das medições. Quando
não há evidência, informa isso explicitamente. A navegação utiliza os mesmos
atalhos e a mesma paleta do restante da interface.

## Limitações

O armazenamento detalhado funciona enquanto a estação de supervisão recebe
os eventos. Interrupções no computador ou na serial criam lacunas observacionais.
Os horários CSV correspondem aos horários de recebimento na TUI (quando
existirem); carimbos de sensor/gateway são armazenados separadamente.
Sincronização temporal distribuída não foi implementada. Não há captura de
áudio bruto, aceleração bruta ou espectro FFT contínuo. Não é um registrador
de evidências à prova de perda de energia; falhas ocorridas durante uma gravação
podem deixar fragmentos órfãos não referenciados pelo banco.

## Validação sugerida

Execute `python -m pytest -q tests/test_history_capture_v2.py tests/test_history_capture_screen_v2.py`
e `./Codigo/scripts/test_tui.sh`. Verifique em bancada uma falha física, uma
perda de sessão e uma eleição, e confirme a correlação entre a timeline e os
arquivos efetivamente registrados. Não há modificação de firmware.
