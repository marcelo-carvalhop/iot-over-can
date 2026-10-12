# Livro do projeto

O livro do projeto é escrito a partir da documentação técnica. Em vez de um texto paralelo, que envelheceria a cada mudança, os trechos destinados ao livro são marcados nos próprios documentos e reunidos por um script. Este arquivo define a marcação e o roteiro: que trechos entram, em que capítulo e em que ordem.

## Marcação

Cada trecho fica entre dois comentários HTML, que não aparecem na leitura do Markdown:

```markdown
<!-- livro:inicio id="acustico-hardware" capitulo="Sensor acústico INMP441" -->
## 2. Hardware e ligação
...
<!-- livro:fim id="acustico-hardware" -->
```

| Regra | Motivo |
|---|---|
| `id` em minúsculas e hífens, prefixado pelo capítulo (`acustico-…`) e único em toda a documentação | o roteiro e o script localizam o trecho só pelo `id` |
| `capitulo` igual ao nome do capítulo no roteiro | impede que um trecho seja escrito para um capítulo e listado em outro |
| cada marcador sozinho na linha, fora de tabelas, listas e blocos de código | um comentário entre linhas de tabela quebra a tabela no GitHub; dentro de um bloco de código o marcador é só exemplo e é ignorado |
| trechos não se aninham nem se sobrepõem | cada parágrafo pertence a um único lugar do livro |
| `titulo="..."` (opcional, depois de `capitulo`) troca, no livro, o primeiro título do trecho; num trecho sem título, cria um | o título certo no documento técnico ("## Sensor acústico INMP441" dentro de `desafios_e_resolucoes.md`) pode não servir no capítulo |
| o trecho faz sentido sozinho | no livro ele aparece fora do documento de origem; o script corrige os links relativos, mas não expressões como "acima" ou "na seção anterior" |

O documento técnico continua sendo a fonte: para mudar o texto do livro, edite o trecho marcado. Um trecho novo precisa também de uma linha no roteiro.

## Rascunho e verificação

```bash
python3 Codigo/scripts/extrair_livro.py --verificar   # só confere a marcação e o roteiro
python3 Codigo/scripts/extrair_livro.py               # gera build/livro/livro.md
python3 Codigo/scripts/extrair_livro.py --capitulo "Sensor acústico INMP441" -o build/livro/acustico.md
```

O rascunho segue a ordem do roteiro. Cada capítulo abre com um título de nível 1; os títulos de cada trecho são deslocados para começar no nível 2 e perdem a numeração da seção de origem ("## 2. Hardware e ligação" vira "## Hardware e ligação"), salvo o primeiro, quando o marcador traz `titulo`; os links relativos são reescritos para o local de saída. Antes de cada trecho fica um comentário com o `id` e o arquivo de origem. `build/` não é versionado.

`Front/tests/test_livro.py` roda a verificação com os testes da TUI. Falham o teste: marcador sem par ou malformado, `id` repetido, trecho vazio, trecho fora do roteiro, linha do roteiro sem trecho ou com o arquivo errado, capítulo divergente e marcador dentro de tabela.

## Roteiro

Cada título abaixo é um capítulo, na ordem do livro; a tabela lista os trechos na ordem em que entram.

### Sensor acústico INMP441

O segundo sensor sem fio do projeto, do microfone I2S às análises na TUI.

| Ordem | Trecho | Arquivo | Conteúdo |
|---|---|---|---|
| 1 | `acustico-visao-geral` | [`sensor-acustico/sensor-acustico.md`](sensor-acustico/sensor-acustico.md) | o que o sensor mede, os três modos e o caminho dos dados |
| 2 | `acustico-perfis` | [`arquitetura/arquitetura.md`](arquitetura/arquitetura.md) | perfil do sensor: o que é comum aos dois sensores sem fio e o que muda |
| 3 | `acustico-hardware` | [`sensor-acustico/sensor-acustico.md`](sensor-acustico/sensor-acustico.md) | INMP441, ligação ao Pico, escolha entre Pico W e Pico 2 W |
| 4 | `acustico-aquisicao` | [`sensor-acustico/sensor-acustico.md`](sensor-acustico/sensor-acustico.md) | I2S mestre na PIO, taxas efetivas, DMA em anel e perda de amostras |
| 5 | `acustico-firmware` | [`sensor-acustico/sensor-acustico.md`](sensor-acustico/sensor-acustico.md) | divisão entre os dois núcleos, atraso do DMA e cão de guarda |
| 6 | `acustico-cadeia-de-medicao` | [`sensor-acustico/sensor-acustico.md`](sensor-acustico/sensor-acustico.md) | da palavra I2S à pressão, escalas de tempo e métricas comuns |
| 7 | `acustico-modos` | [`sensor-acustico/sensor-acustico.md`](sensor-acustico/sensor-acustico.md) | LEVEL (exposição e dose), MACHINE (assinatura e fundamental) e EVENTS (STA/LTA) |
| 8 | `acustico-configuracao` | [`sensor-acustico/sensor-acustico.md`](sensor-acustico/sensor-acustico.md) | campos da configuração, calibração e console USB |
| 9 | `acustico-transporte` | [`sensor-acustico/sensor-acustico.md`](sensor-acustico/sensor-acustico.md) | relatório de um segundo, caminho até a TUI e comandos pela rede |
| 10 | `acustico-protocolo` | [`protocolo/protocolo.md`](protocolo/protocolo.md) | tipo `7` no CAN, opcode `0x21`, perfil na configuração e recusa por perfil |
| 11 | `acustico-diagnostico` | [`sensor-acustico/sensor-acustico.md`](sensor-acustico/sensor-acustico.md) | DTCs do microfone, da aquisição e do processamento |
| 12 | `acustico-tui` | [`sensor-acustico/sensor-acustico.md`](sensor-acustico/sensor-acustico.md) | análises da TUI por modo: NR-15/NHO-01, curvas NC, conversa, tacômetro, eventos |
| 13 | `acustico-tela-sensor` | [`interface/tui.md`](interface/tui.md) | como a tela do sensor apresenta essas análises |
| 14 | `acustico-build` | [`build_e_teste.md`](build_e_teste.md) | compilar, gravar e conferir o sensor acústico |
| 15 | `acustico-validacao` | [`sensor-acustico/sensor-acustico.md`](sensor-acustico/sensor-acustico.md) | testes nativos, simulação da PIO, bancada virtual e TUI |
| 16 | `acustico-desafios` | [`desafios_e_resolucoes.md`](desafios_e_resolucoes.md) | problemas encontrados no caminho e como foram resolvidos |
| 17 | `acustico-limitacoes` | [`sensor-acustico/sensor-acustico.md`](sensor-acustico/sensor-acustico.md) | o que ainda não foi medido em bancada e os próximos passos |
