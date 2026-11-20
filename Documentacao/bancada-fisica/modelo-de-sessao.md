# Modelo de registro de sessão de bancada

Uma sessão de bancada é um período de testes seguidos com as mesmas placas e o mesmo commit. Se o commit mudar no meio, abra outra sessão.

Para registrar uma sessão, copie o modelo (a partir da linha `# Sessão de bancada física`) para `Documentacao/historico/bancada-fisica-AAAA-MM-DD.md`, preencha e apague as instruções em itálico. Quem ler o registro daqui a meses não terá o caderno: o que importa precisa estar no registro ou nos arquivos que ele cita.

Regras do preenchimento:

- O resultado de um RT é `passou`, `falhou`, `parcial` ou `não feito`, com o motivo quando não for `passou`. Nunca em branco.
- Toda medida leva unidade e o número de repetições. Uma medida única é dita como única.
- O que não foi visto não entra como «passou». Vai para «Ainda não validado em bancada física».
- Arquivos grandes (gravações da sessão, exportações) não entram no repositório: cite onde ficaram e junte só os trechos e as saídas dos programas.
- Se algo divergir do [roteiro](roteiro-de-testes.md) (saída diferente da esperada, comando que mudou), corrija o roteiro no mesmo commit do registro.

---

# Sessão de bancada física — AAAA-MM-DD

## Resumo

*Três linhas: o que se quis validar, o que se conseguiu e o que decide a próxima sessão.*

## Identificação

| Item | Valor |
| --- | --- |
| Data e horário | AAAA-MM-DD, hh:mm a hh:mm (fuso) |
| Quem executou | nome |
| Commit | saída de `git log -1 --format=%h`, sem `-dirty` |
| Versão | saída de `cat VERSION` |
| `versao.py --check` | OK ou o que acusou |
| Roteiro | [roteiro-de-testes.md](../bancada-fisica/roteiro-de-testes.md), no commit `<hash>` |
| RT cobertos | por exemplo: 1 a 19 e 71 |

## Montagem

*Uma linha por peça. Em «Gravado», o que a própria placa disse: `FIRMWARE=`, `COMMIT=` e as opções de compilação fora do padrão (`--auth-can`, esperas do sensor, medição de energia, pinos do microfone, `--sem-chave`).*

| Peça | Identificação | Gravado |
| --- | --- | --- |
| Probe 00 | ESP32 + MCP2515 (cristal de ... MHz) | |
| Módulos CAN | IDs ... | |
| Sensor de vibração | Pico W, `NODE_UUID=0x...` | |
| Sensor acústico | Pico W ou Pico 2 W, `NODE_UUID=0x...` | |
| Painel e leitor | | |
| Alimentação | fonte, cabos, hub USB | |
| Barramento | comprimento, terminações de 120 Ω (onde), GND comum | |
| Chaves | `LINK_KEYS=MASTER` ou `DEVICE` em cada sensor | |
| Estação | sistema, versão do Python, modo de operador | |

## Resultado por RT

*Uma linha por RT feito ou tentado. «Medido» é o que a linha **Registro** do RT pede.*

| RT | Resultado | Medido | Observação |
| --- | --- | --- | --- |
| 1 | passou | | |

## Medidas contra a bancada virtual

*Preencha com a saída de `python3 Simulacao/comparar_bancada.py <gravação> --cenario <cenário>`, uma linha por medida. Se o resultado for `FORA`, a bancada virtual não descreve a montagem: registre o tempo medido em cada etapa.*

| Medida | Física (mediana de n; pior caso) | Bancada virtual | Resultado |
| --- | --- | --- | --- |
| `troca_de_lider_ms` | | | |
| `reassociacao_ms` | | | |
| `abertura_do_canal_ms` | | | |

*Outros tempos que o roteiro pede: isolamento detectado e volta (RT 71), eleição sem a Probe (RT 34), desistência da sessão pelo sensor (RT 60).*

## O que funcionou

*Uma linha por função, com o RT.*

## O que falhou ou ficou em aberto

*Para cada item: o sintoma, o RT em que apareceu, o que foi tentado e a hipótese. Se virou desafio, o número `CH-` em [Desafios](../desafios.md).*

## Ainda não validado em bancada física

*O que o roteiro pede e a sessão não fez, e por quê.*

## Arquivos

| Arquivo | O que é e onde está |
| --- | --- |
| `logs/gravacao_*.log` | gravação da Probe 00 (`--record-session`) |
| `exports/` | exportações da estação |
| linha `CONFIG` (RT 83) | parâmetros de compilação da Probe 00 |
| `operadores.sh audit --since 24h` | saída, com o número de registros do `audit --verify` e as âncoras de sessão conferidas |
| `smoke_boot.py` | saída de cada placa (`SMOKE RESULT=`) |

## Mudanças que a sessão pede no repositório

| Mudança | Motivo | Situação |
| --- | --- | --- |
| | | |
