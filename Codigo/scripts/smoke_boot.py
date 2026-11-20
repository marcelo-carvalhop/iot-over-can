#!/usr/bin/env python3
"""Verificação de fumaça depois de gravar uma placa.

Abre a porta serial (ou lê uma captura de texto), confere as linhas que o firmware escreve na partida e
nos comandos de identificação, e devolve aprovado ou reprovado. Serve para pegar na hora o que antes só
aparecia no meio de um ensaio: placa gravada sem a chave do enlace (``LINK_KEY=MISSING``), versões
misturadas na bancada, sensor sem o MPU-6050 ou sem microfone, controlador CAN que não inicia.

    smoke_boot.py probe    --porta /dev/ttyUSB0
    smoke_boot.py modulo   --porta /dev/ttyUSB1 --no 2 [--selftest]
    smoke_boot.py vibracao --porta /dev/ttyACM0
    smoke_boot.py acustico --porta /dev/ttyACM1
    smoke_boot.py painel   --porta /dev/ttyUSB2        (CYD)
    smoke_boot.py leitor   --porta /dev/ttyACM2        (M5Stack PaperColor)
    smoke_boot.py modulo   --no 2 --arquivo captura.log   (confere um texto já capturado, sem placa)

Probe, módulos, painel e leitor reiniciam ao abrir a porta (``--reset`` força) e a partida inteira é
lida; os sensores Pico não têm partida a capturar pela USB e respondem aos comandos VERSION, STATUS e NET.

A versão e o commit gravados são comparados com o arquivo ``VERSION`` do repositório e entre si, de modo
que uma placa com firmware antigo, ou duas de versões diferentes na mesma captura, reprovam.

Saída: uma linha por verificação (OK, AVISO ou FALHA) e, por último, ``SMOKE RESULT=PASS|WARN|FAIL ...``,
própria para o script de quem chama. Código de saída: 0 aprovado, 1 reprovado (ou aviso com ``--estrito``),
2 uso incorreto, 3 nenhuma linha recebida da placa.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Protocol, Sequence

ROOT = Path(__file__).resolve().parents[2]

PROTOCOL = "CAN_CLASSIC_V1"
# Recursos que a Probe anuncia (Codigo/node-can/include/firmware_info.h; um teste confere).
FEATURES = ("WIRELESS_DATA", "ACOUSTIC_DATA", "BUS_METRICS", "FAULT_INJECTION", "FAILOVER", "RESEARCH", "TIME_SYNC")

OK, WARN, FAIL = "OK", "AVISO", "FALHA"

KEY_HINT = (
    "a placa foi gravada sem a chave do enlace (CH-056): rode ./Codigo/scripts/provision_sensor_security.sh "
    "--add-link-key e grave de novo"
)
CRASH = r"Guru Meditation Error|abort\(\) was called|assert failed|Backtrace:"
BROWNOUT = r"rst:0x0?f \(BROWNOUT_RST\)|Brownout detector was triggered|\[ENERGIA\] ATENCAO"
UUID = r"0x[0-9A-Fa-f]{16}"

MAIN_INO = "Codigo/node-can/src/main.ino"
WLINK = "Codigo/node-can/src/wireless_link.cpp"
SENSOR_CONSOLE = "Codigo/node-wifi/serial_console.c"
AUDIO_CONSOLE = "Codigo/node-audio/audio_console.c"


@dataclass(frozen=True)
class Check:
    """Uma linha que precisa aparecer na captura."""

    name: str
    pattern: str                              # expressão regular, procurada em cada linha
    hint: str                                 # o que fazer quando a linha não aparece
    source: tuple[str, str] | None = None     # (arquivo, trecho) do firmware que escreve a linha; um teste confere
    required: bool = True


@dataclass(frozen=True)
class Forbidden:
    """Uma linha que não pode aparecer; ``explains`` é a verificação cuja falta ela explica."""

    name: str
    pattern: str
    hint: str
    explains: str = ""
    level: str = FAIL


@dataclass(frozen=True)
class Plan:
    role: str
    checks: tuple[Check, ...]
    forbidden: tuple[Forbidden, ...]
    boot_done: str | None = None                          # linha que encerra a partida; None: não há partida a ler
    commands: tuple[tuple[str, str], ...] = ()            # (comando, linha que encerra a resposta)
    resets: bool = False                                  # reinicia a placa antes de ler (ESP32)
    settle: float = 0.6                                   # sem partida a ler: segundos de leitura antes dos comandos


@dataclass
class Result:
    name: str
    level: str
    detail: str = ""
    hint: str = ""


@dataclass
class Report:
    role: str
    results: list[Result] = field(default_factory=list)
    firmware: str = "-"
    commit: str = "-"

    def add(self, name: str, level: str, detail: str = "", hint: str = "") -> None:
        self.results.append(Result(name, level, detail, hint))

    def count(self, level: str) -> int:
        return sum(1 for item in self.results if item.level == level)

    @property
    def verdict(self) -> str:
        if self.count(FAIL):
            return "FAIL"
        return "WARN" if self.count(WARN) else "PASS"


# --------------------------------------------------------------------------- planos por papel


def _crash_rules(node: int) -> tuple[Forbidden, ...]:
    return (
        Forbidden("controlador-can", rf"\[NODE {node}\] \[ERRO\] Falha ao iniciar CAN",
                  "o MCP2515 não respondeu: confira os fios (CS, SCK, MOSI, MISO, INT), a alimentação de 5 V e o cristal",
                  explains="can-iniciado"),
        Forbidden("queda-de-tensao", BROWNOUT, "a placa reiniciou por queda de tensão: troque o cabo USB ou a fonte"),
        Forbidden("falha-do-programa", CRASH, "o programa falhou na partida: grave de novo e guarde a saída serial"),
    )


def plan_probe() -> Plan:
    return Plan(
        "probe",
        checks=(
            Check("partida", r"Iniciando NODE 0\b", "a Probe não imprimiu a partida: confira a porta e a velocidade (115200)",
                  (MAIN_INO, 'Serial.print("Iniciando NODE ")')),
            Check("versao-na-partida",
                  r"\[NODE 0\] \[BOOT\] FIRMWARE=\S+ COMMIT=\S+ PROTOCOL=" + PROTOCOL + r" ROLE=PROBE",
                  "firmware sem a linha [BOOT]: grave de novo com a versão atual",
                  (MAIN_INO, "[NODE %u] [BOOT] FIRMWARE=%s COMMIT=%s PROTOCOL=%s ROLE=%s")),
            Check("papel", r"\[PROBE 00\] CAPS=CAN_MONITOR,SERIAL_INSTRUMENTATION LOCAL_PROFILE=NONE",
                  "gravada com outro número: a Probe é o ID 0 (upload_esp32_can_node.sh 0 ...)",
                  (MAIN_INO, "[PROBE 00] CAPS=CAN_MONITOR,SERIAL_INSTRUMENTATION LOCAL_PROFILE=NONE")),
            Check("can-iniciado", r"\[GW\] Probe 00 ativada",
                  "o controlador CAN não iniciou: confira os fios do MCP2515 e a alimentação",
                  (MAIN_INO, "[GW] Probe 00 ativada")),
            Check("serial-pronta", r"\[GW\] Interface serial da Probe 00 pronta", "a Probe não terminou de iniciar",
                  (MAIN_INO, "[GW] Interface serial da Probe 00 pronta")),
            Check("recursos", r"\[PROBE 00\] FIRMWARE=\S+ COMMIT=\S+ FEATURES=(?P<features>\S+)",
                  "firmware sem a linha de recursos: grave de novo com a versão atual",
                  (MAIN_INO, '[PROBE 00] FIRMWARE=" IOT_FW_VERSION')),
            Check("versao-por-comando", r"^PROBE_VERSION FIRMWARE=\S+ COMMIT=\S+ PROTOCOL=" + PROTOCOL + r" NODE=0 FEATURES=\S+",
                  "a Probe não respondeu ao comando PROBE_VERSION",
                  ("Codigo/node-can/src/comandos.cpp", "PROBE_VERSION FIRMWARE=")),
            Check("estado", r"^PROBE_STATUS NODE=0 STATE=ONLINE CAN=CLASSIC ARB=500000 DATA=0 WIFI=OFF BLE_SCAN=OFF UPTIME_MS=\d+",
                  "a Probe não respondeu ao comando PROBE_STATUS",
                  ("Codigo/node-can/src/comandos.cpp", "PROBE_STATUS NODE=0 STATE=ONLINE")),
        ),
        forbidden=_crash_rules(0),
        boot_done=r"\[PROBE 00\] FIRMWARE=\S+ COMMIT=\S+ FEATURES=",
        commands=(("PROBE_VERSION", r"^PROBE_VERSION "), ("PROBE_STATUS", r"^PROBE_STATUS ")),
        resets=True,
    )


def plan_module(node: int, selftest: bool = False) -> Plan:
    tag = rf"\[NODE {node}\]"
    checks = [
        Check("partida", rf"Iniciando NODE {node}\b", "o módulo não imprimiu a partida: confira a porta e a velocidade (115200)",
              (MAIN_INO, 'Serial.print("Iniciando NODE ")')),
        Check("versao-na-partida",
              tag + r" \[BOOT\] FIRMWARE=\S+ COMMIT=\S+ PROTOCOL=" + PROTOCOL + r" ROLE=NODE",
              "firmware sem a linha [BOOT] (ou gravado com o número 0, que é o da Probe): grave de novo com a versão atual",
              (MAIN_INO, "[NODE %u] [BOOT] FIRMWARE=%s COMMIT=%s PROTOCOL=%s ROLE=%s")),
        Check("leds", r"\[LEDS\] IDLE", "o módulo não chegou ao fim da inicialização dos LEDs",
              (MAIN_INO, "[LEDS] IDLE: todos acesos")),
        Check("papel", tag + r" CAPS=CAN,LOCAL_SENSOR,LOCAL_SENSOR_DEMO,BLE_SCAN LOCAL_PROFILE=DEMO_BYTE",
              "gravado com outro número: confira o argumento de upload_esp32_can_node.sh",
              (MAIN_INO, "] CAPS=CAN,LOCAL_SENSOR,LOCAL_SENSOR_DEMO,BLE_SCAN LOCAL_PROFILE=DEMO_BYTE")),
        Check("chave-do-enlace", tag + r" \[WLINK\] LINK_KEY=PROVISIONED", KEY_HINT,
              (WLINK, "[NODE %u] [WLINK] LINK_KEY=%s")),
        Check("can-iniciado", tag + r" \[INIT\] Pronto\.",
              "o controlador CAN não iniciou: confira os fios do MCP2515 e a alimentação",
              (MAIN_INO, "] [INIT] Pronto. Aguardando eleicao manual ou heartbeat para JOIN")),
        Check("versao-por-comando", rf"^NODE_VERSION NODE={node} FIRMWARE=\S+ COMMIT=\S+ PROTOCOL=" + PROTOCOL + r" FEATURES=\S+",
              "o módulo não respondeu ao comando VERSION",
              ("Codigo/node-can/src/comandos.cpp", "NODE_VERSION NODE=%u FIRMWARE=%s COMMIT=%s")),
    ]
    forbidden = [
        Forbidden("chave-ausente", tag + r" \[WLINK\] LINK_KEY=MISSING", KEY_HINT, explains="chave-do-enlace"),
        *_crash_rules(node),
    ]
    commands = [("VERSION", r"^NODE_VERSION ")]
    if selftest:
        checks.append(Check("autoteste", tag + r" \[SELFTEST\] RESULT=PASS\b",
                            "o autoteste não passou: veja as linhas [SELFTEST] acima",
                            ("Codigo/node-can/src/autoteste.cpp", "[NODE %u] [SELFTEST] RESULT=%s")))
        forbidden += [
            Forbidden("autoteste-recusado", tag + r" \[SELFTEST\] RECUSADO",
                      "o módulo já entrou na rede: rode o autoteste antes de ligar o barramento", explains="autoteste"),
            Forbidden("autoteste-falhou", tag + r" \[SELFTEST\] RESULT=FAIL",
                      "o controlador CAN reprovou no autoteste: veja a linha DICA logo acima", explains="autoteste"),
            Forbidden("autoteste-aviso", tag + r" \[SELFTEST\] RESULT=WARN",
                      "o autoteste passou com aviso: veja a linha DICA logo acima", explains="autoteste", level=WARN),
        ]
        commands.append(("SELFTEST CAN", r"\[SELFTEST\] (?:RESULT=|RECUSADO|uso:)"))
    return Plan("modulo", tuple(checks), tuple(forbidden), boot_done=tag + r" \[INIT\] Pronto\.",
                commands=tuple(commands), resets=True)


def _sensor_common(console: str) -> tuple[list[Check], list[Forbidden]]:
    checks = [
        Check("chaves-do-enlace", r"^NET STATE=\S+ .*\bLINK_KEYS=(?:MASTER|DEVICE)\b", KEY_HINT, (console, "LINK_KEYS=%s")),
        Check("anuncio-ble", r"^NET STATE=\S+ .*\bBLE_BEACON=ON\b",
              "o sensor não está anunciando por BLE: nenhum módulo vai encontrá-lo", (console, "BLE_BEACON=%s")),
        Check("console-travado", r"^STATUS .*\bAUTH=LOCKED\b",
              "o console aceita comandos que alteram o sensor sem senha: confira EDGE_SERIAL_ADMIN_TOKEN", (console, "AUTH=%s")),
        Check("sem-falhas", r"^STATUS .*\bDTC=0x0000\b",
              "o sensor registrou um código de falha (DTC): veja o campo DTC na linha STATUS", (console, "DTC=0x%04X")),
    ]
    forbidden = [
        Forbidden("sem-chave", r"^NET STATE=\S+ .*\bLINK_KEYS=NONE\b", KEY_HINT, explains="chaves-do-enlace"),
        Forbidden("chave-de-outro-sensor", r"^NET STATE=\S+ .*\bLINK_KEYS=UUID_MISMATCH\b",
                  "as chaves gravadas são de outro sensor: compile com --sensor <UUID deste sensor>",
                  explains="chaves-do-enlace"),
        Forbidden("sem-anuncio", r"^NET STATE=\S+ .*\bBLE_BEACON=OFF\b",
                  "o anúncio BLE está desligado: o sensor não será encontrado", explains="anuncio-ble"),
        Forbidden("console-destravado", r"^STATUS .*\bAUTH=(?!LOCKED\b)\S+",
                  "o console não está travado: confira EDGE_SERIAL_ADMIN_TOKEN", explains="console-travado"),
        Forbidden("falha-registrada", r"^STATUS .*\bDTC=0x(?!0000\b)[0-9A-Fa-f]{4}\b",
                  "o sensor registrou um código de falha (DTC): veja o campo DTC na linha STATUS", explains="sem-falhas"),
    ]
    return checks, forbidden


_SENSOR_COMMANDS = (("VERSION", r"^VERSION "), ("STATUS", r"^STATUS "), ("NET", r"^NET STATE="))


def plan_vibration() -> Plan:
    checks, forbidden = _sensor_common(SENSOR_CONSOLE)
    checks = [
        Check("versao-por-comando", r"^VERSION FIRMWARE=\S+ COMMIT=\S+ PROTOCOL=\d+ NODE_UUID=" + UUID + r"\s*$",
              "o sensor não respondeu ao comando VERSION (firmware anterior à 0.22.0?)",
              (SENSOR_CONSOLE, "VERSION FIRMWARE=%s COMMIT=%s PROTOCOL=%u NODE_UUID=0x%016llX")),
        Check("mpu", r"^STATUS .*\bMPU=YES\b",
              "o MPU-6050 não respondeu: confira SDA, SCL, VCC e o endereço I2C (AD0)", (SENSOR_CONSOLE, "MPU=%s")),
        *checks,
        Check("partida", r"^BOOT FIRMWARE=\S+ COMMIT=\S+ PROTOCOL=\d+ PROFILE=VIBRATION",
              "sem a linha de partida (normal se a porta foi aberta depois de o sensor ligar)",
              (SENSOR_CONSOLE, "BOOT FIRMWARE=%s COMMIT=%s PROTOCOL=%u PROFILE=VIBRATION"), required=False),
    ]
    forbidden = [
        Forbidden("firmware-acustico", r"^VERSION .*PROFILE=ACOUSTIC",
                  "esta placa tem o firmware do sensor acústico; para o de vibração use build_pico.sh sem --audio",
                  explains="versao-por-comando"),
        Forbidden("sem-mpu", r"^STATUS .*\bMPU=NO\b",
                  "o MPU-6050 não respondeu: confira SDA, SCL, VCC e o endereço I2C (AD0)", explains="mpu"),
        *forbidden,
    ]
    return Plan("vibracao", tuple(checks), tuple(forbidden), commands=_SENSOR_COMMANDS)


def plan_acoustic() -> Plan:
    checks, forbidden = _sensor_common(AUDIO_CONSOLE)
    checks = [
        Check("versao-por-comando",
              r"^VERSION FIRMWARE=\S+ COMMIT=\S+ PROTOCOL=\d+ NODE_UUID=" + UUID + r" PROFILE=ACOUSTIC CHIP=(?:RP2040|RP2350) BOARD=\S+",
              "o sensor não respondeu ao comando VERSION (ou é o firmware de vibração: use build_pico.sh --audio)",
              (AUDIO_CONSOLE, "VERSION FIRMWARE=%s COMMIT=%s PROTOCOL=%u NODE_UUID=0x%016llX PROFILE=ACOUSTIC")),
        Check("microfone", r"^STATUS PROFILE=ACOUSTIC .*\bMIC=OK\b",
              "o microfone não está OK: confira VDD, GND, SD, SCK, WS e o pino L/R do INMP441", (AUDIO_CONSOLE, "MIC=%s")),
        Check("cobertura", r"^STATUS PROFILE=ACOUSTIC .*\bCOVERAGE_PCT=100\b",
              "a captura do microfone perdeu amostras (COVERAGE_PCT abaixo de 100)", (AUDIO_CONSOLE, "COVERAGE_PCT=%u")),
        *checks,
        Check("partida", r"^BOOT FIRMWARE=\S+ COMMIT=\S+ PROTOCOL=\d+ PROFILE=ACOUSTIC CHIP=\S+",
              "sem a linha de partida (normal se a porta foi aberta depois de o sensor ligar)",
              (AUDIO_CONSOLE, "BOOT FIRMWARE=%s COMMIT=%s PROTOCOL=%u PROFILE=ACOUSTIC CHIP=%s"), required=False),
    ]
    forbidden = [
        Forbidden("microfone-ausente", r"^STATUS PROFILE=ACOUSTIC .*\bMIC=NO_HARDWARE\b",
                  "a captura I2S (PIO) do microfone não iniciou: confira SCK, WS (SCK + 1) e SD (GP18, GP19 e GP20 por padrão) e se --mic-pins escolheu GPIO livres",
                  explains="microfone"),
        Forbidden("microfone-mudo", r"^STATUS PROFILE=ACOUSTIC .*\bMIC=SILENT\b",
                  "o microfone só entrega zeros: confira VDD (3,3 V) e o pino L/R", explains="microfone"),
        Forbidden("microfone-preso", r"^STATUS PROFILE=ACOUSTIC .*\bMIC=STUCK_HIGH\b",
                  "o microfone entrega só uns: confira SD em curto com 3,3 V e o pino L/R", explains="microfone"),
        *forbidden,
    ]
    # Logo depois de gravar, o microfone ainda não mediu uma janela inteira (COVERAGE_PCT=0): espera um pouco.
    return Plan("acustico", tuple(checks), tuple(forbidden), commands=_SENSOR_COMMANDS, settle=2.0)


def plan_panel() -> Plan:
    return Plan(
        "painel",
        checks=(
            Check("pronto", r"#IOC (?:READY|HELLO) uuid=" + UUID + r" profile=PANEL fw=\S+ commit=\S+",
                  "o painel não anunciou a partida: confira a porta e a velocidade",
                  ("Codigo/node-paper/src/cyd/main_cyd.cpp", "#IOC READY uuid=0x%016llX profile=PANEL fw=%s commit=%s")),
        ),
        forbidden=(
            Forbidden("queda-de-tensao", BROWNOUT, "a placa reiniciou por queda de tensão: troque o cabo USB ou a fonte"),
            Forbidden("falha-do-programa", CRASH, "o programa falhou na partida: grave de novo e guarde a saída serial"),
        ),
        boot_done=r"#IOC (?:READY|HELLO) ",
        resets=True,
    )


def plan_reader() -> Plan:
    return Plan(
        "leitor",
        checks=(
            Check("firmware", r"#LEITOR firmware \S+ commit \S+", "o leitor não anunciou a partida: confira a porta",
                  ("Codigo/leitor/src/main.cpp", '#LEITOR firmware %s commit %s')),
            Check("documentos", r"#LEITOR documentação \S+, \d+ documentos",
                  "o leitor não carregou os documentos: regenere Codigo/leitor/src/documentos.h com tools/documentos.py",
                  ("Codigo/leitor/src/main.cpp", "#LEITOR documentação %s, %u documentos")),
        ),
        forbidden=(
            Forbidden("queda-de-tensao", BROWNOUT, "a placa reiniciou por queda de tensão: troque o cabo USB ou a fonte"),
            Forbidden("falha-do-programa", CRASH, "o programa falhou na partida: grave de novo e guarde a saída serial"),
        ),
        boot_done=r"#LEITOR documentação ",
        resets=True,
    )


ROLES = ("probe", "modulo", "vibracao", "acustico", "painel", "leitor")


def build_plan(role: str, node: int | None = None, selftest: bool = False) -> Plan:
    if role == "modulo":
        if node is None or not 1 <= node <= 31:
            raise ValueError("o papel 'modulo' precisa de --no <1 a 31>")
        return plan_module(node, selftest)
    if selftest and role != "probe":
        raise ValueError("--selftest vale só para 'modulo' e 'probe'")
    if role == "probe":
        plan = plan_probe()
        if not selftest:
            return plan
        selftest_check = Check("autoteste", r"\[NODE 0\] \[SELFTEST\] RESULT=PASS\b",
                               "o autoteste não passou: veja as linhas [SELFTEST] acima",
                               ("Codigo/node-can/src/autoteste.cpp", "[NODE %u] [SELFTEST] RESULT=%s"))
        rules = (
            Forbidden("autoteste-falhou", r"\[NODE 0\] \[SELFTEST\] RESULT=FAIL",
                      "o controlador CAN reprovou no autoteste: veja a linha DICA logo acima", explains="autoteste"),
            Forbidden("autoteste-aviso", r"\[NODE 0\] \[SELFTEST\] RESULT=WARN",
                      "o autoteste passou com aviso: veja a linha DICA logo acima", explains="autoteste", level=WARN),
        )
        return Plan(plan.role, plan.checks + (selftest_check,), plan.forbidden + rules, plan.boot_done,
                    plan.commands + (("SELFTEST CAN", r"\[SELFTEST\] (?:RESULT=|RECUSADO|uso:)"),), plan.resets)
    factories: dict[str, Callable[[], Plan]] = {
        "vibracao": plan_vibration, "acustico": plan_acoustic, "painel": plan_panel, "leitor": plan_reader,
    }
    if role not in factories:
        raise ValueError(f"papel desconhecido: {role} (use {', '.join(ROLES)})")
    return factories[role]()


# --------------------------------------------------------------------------- versão e commit

_FIRMWARE = (r"\bFIRMWARE=(\S+)", r"\bfw=(\S+)", r"#LEITOR firmware (\S+) commit")
_COMMIT = (r"\bCOMMIT=(\S+)", r"\bcommit=(\S+)", r"#LEITOR firmware \S+ commit (\S+)")
# A bancada virtual e as gravações de Front/replays prefixam cada linha com o instante simulado.
_PREFIX = re.compile(r"^(?:#N\d+)?@\d+ ")
_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def normalize(raw: str) -> str:
    return _PREFIX.sub("", _ANSI.sub("", raw.replace("\r", "")).strip())


def _values(patterns: Sequence[str], lines: Sequence[str]) -> list[str]:
    found: list[str] = []
    for line in lines:
        for pattern in patterns:
            match = re.search(pattern, line)
            if match:
                found.append(match.group(1))
                break
    return found


def read_expected_version(root: Path = ROOT) -> str | None:
    path = root / "VERSION"
    return path.read_text(encoding="utf-8").strip() if path.is_file() else None


def current_commit(root: Path = ROOT) -> str | None:
    """Commit curto do repositório (o mesmo critério do firmware: 8 caracteres), ou None fora do git."""

    try:
        head = subprocess.run(["git", "-C", str(root), "rev-parse", "--short=8", "HEAD"],
                              capture_output=True, text=True, check=False)
    except OSError:
        return None
    return head.stdout.strip() if head.returncode == 0 and head.stdout.strip() else None


def check_versions(report: Report, lines: Sequence[str], expected_version: str | None,
                   expected_commit: str | None) -> None:
    versions = _values(_FIRMWARE, lines)
    commits = _values(_COMMIT, lines)
    if not versions:
        report.add("versao", FAIL, "nenhuma linha com a versão do firmware",
                   "firmware antigo, de antes de a versão ser impressa? Grave de novo pelos scripts de Codigo/scripts/")
        return
    distinct = sorted(set(versions))
    report.firmware = ",".join(distinct)
    if len(distinct) > 1:
        report.add("versao-unica", FAIL, f"versões diferentes na captura: {', '.join(distinct)}",
                   "grave todas as placas com a mesma versão")
    elif distinct[0].startswith("0.0.0"):
        report.add("versao-definida", FAIL, f"versão {distinct[0]}",
                   "o firmware foi compilado sem a versão: compile pelos scripts de Codigo/scripts/ (ou pelo PlatformIO do projeto)")
    elif expected_version and distinct[0] != expected_version:
        report.add("versao-esperada", FAIL, f"gravado {distinct[0]}, o arquivo VERSION diz {expected_version}",
                   "grave de novo com o código atual, ou confirme se é essa mesmo a versão que se quer")
    else:
        report.add("versao", OK, distinct[0])

    if not commits:
        report.add("commit", FAIL, "nenhuma linha com o commit do firmware", "grave de novo pelos scripts de Codigo/scripts/")
        return
    distinct_commits = sorted(set(commits))
    report.commit = ",".join(distinct_commits)
    if len(distinct_commits) > 1:
        report.add("commit-unico", FAIL, f"commits diferentes na captura: {', '.join(distinct_commits)}",
                   "grave todas as placas a partir do mesmo commit")
        return
    commit = distinct_commits[0]
    if commit == "unknown":
        report.add("commit", WARN, "commit desconhecido", "o firmware foi compilado fora de um repositório git")
    elif commit.endswith("-dirty"):
        report.add("commit", WARN, commit, "o firmware gravado tem alterações que não estão em nenhum commit")
    else:
        report.add("commit", OK, commit)
    if expected_commit and commit != "unknown" and not commit.startswith(expected_commit):
        report.add("commit-esperado", FAIL, f"gravado {commit}, esperado {expected_commit}",
                   "grave de novo a partir do commit que se quer conferir")


# --------------------------------------------------------------------------- avaliação


def evaluate(plan: Plan, lines: Sequence[str], *, expected_version: str | None = None,
             expected_commit: str | None = None) -> Report:
    """Confere as linhas capturadas contra o plano do papel. Não faz E/S."""

    report = Report(plan.role)
    # O que não pode aparecer vem primeiro: explica a falta da linha esperada e evita duas falhas pelo mesmo defeito.
    explained: set[str] = set()
    for rule in plan.forbidden:
        hit = next((line for line in lines if re.search(rule.pattern, line)), None)
        if hit is None:
            continue
        report.add(rule.name, rule.level, hit, rule.hint)
        explained.add(rule.explains)

    for check in plan.checks:
        line, match = "", None
        for candidate in lines:
            match = re.search(check.pattern, candidate)
            if match:
                line = candidate
                break
        if match:
            if check.name == "recursos":
                missing = [item for item in FEATURES if item not in match.group("features").split(",")]
                if missing:
                    report.add(check.name, FAIL, line, f"firmware sem os recursos {', '.join(missing)}: grave a versão atual")
                    continue
            report.add(check.name, OK, line)
        elif check.name in explained:
            continue
        elif check.required:
            report.add(check.name, FAIL, "linha não encontrada", check.hint)
        else:
            report.add(check.name, OK, "não capturada", check.hint)
    check_versions(report, lines, expected_version, expected_commit)
    return report


def format_report(report: Report, source: str) -> str:
    out = [f"== Verificação de fumaça: {report.role} ({source}) =="]
    for item in report.results:
        out.append(f"{item.level:<6} {item.name:<20} {item.detail}".rstrip())
        if item.hint and item.level != OK:
            out.append(f"       -> {item.hint}")
    out.append(f"SMOKE RESULT={report.verdict} ROLE={report.role} CHECKS={len(report.results)} "
               f"WARN={report.count(WARN)} FAIL={report.count(FAIL)} FIRMWARE={report.firmware} COMMIT={report.commit}")
    return "\n".join(out)


# --------------------------------------------------------------------------- captura


class LinePort(Protocol):
    def readline(self) -> bytes: ...
    def write(self, data: bytes) -> object: ...
    def pulse_reset(self) -> None: ...


def collect(plan: Plan, port: LinePort, *, boot_seconds: float = 12.0, command_seconds: float = 4.0,
            reset: bool | None = None, clock: Callable[[], float] | None = None) -> list[str]:
    """Lê a partida da placa e envia os comandos do plano; devolve as linhas, já normalizadas."""

    lines: list[str] = []
    clock = clock or time.monotonic

    def pump(done: Callable[[str], bool] | None, seconds: float) -> bool:
        end = clock() + seconds
        while clock() < end:
            raw = port.readline()
            if not raw:
                continue
            line = normalize(raw.decode("utf-8", "replace"))
            lines.append(line)
            if done is not None and done(line):
                return True
        return False

    if plan.resets if reset is None else reset:
        port.pulse_reset()
    if plan.boot_done:
        pattern = re.compile(plan.boot_done)
        if pump(lambda line: bool(pattern.search(line)), boot_seconds):
            pump(None, 0.4)   # o que a partida ainda escreve depois da última linha esperada
    else:
        pump(None, plan.settle)   # sensor Pico: pode haver uma partida em curso; o microfone leva um instante a medir
    for command, until in plan.commands:
        port.write(command.encode("ascii") + b"\n")
        pattern = re.compile(until)
        pump(lambda line: bool(pattern.search(line)), command_seconds)
    return lines


class SerialPort:
    """A porta serial real (pyserial); só ela precisa de hardware."""

    def __init__(self, path: str, baud: int) -> None:
        try:
            import serial
        except ImportError as error:  # pragma: no cover - depende do ambiente
            raise SystemExit("smoke_boot.py: instale o pyserial (pip install pyserial, ou use Front/.venv)") from error
        self._serial = serial.Serial(path, baud, timeout=0.2)

    def readline(self) -> bytes:
        return self._serial.readline()

    def write(self, data: bytes) -> object:
        return self._serial.write(data)

    def pulse_reset(self) -> None:
        # Placas ESP32: RTS liga o pino EN (reinício) e DTR o IO0 (modo de gravação); DTR solto = partida normal.
        self._serial.reset_input_buffer()
        self._serial.dtr = False
        self._serial.rts = True
        time.sleep(0.1)
        self._serial.rts = False
        time.sleep(0.2)

    def close(self) -> None:
        self._serial.close()


# --------------------------------------------------------------------------- linha de comando


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Verificação de fumaça depois de gravar uma placa.")
    parser.add_argument("papel", choices=ROLES, help="o que foi gravado na placa")
    origin = parser.add_mutually_exclusive_group(required=True)
    origin.add_argument("--porta", help="porta serial da placa, por exemplo /dev/ttyUSB0")
    origin.add_argument("--arquivo", help="captura de texto já feita ('-' lê da entrada padrão)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--no", type=int, help="número do módulo (1 a 31), para o papel 'modulo'")
    parser.add_argument("--selftest", action="store_true",
                        help="manda SELFTEST CAN (laço interno do MCP2515) e exige RESULT=PASS; só com módulo ou Probe fora da rede")
    reset = parser.add_mutually_exclusive_group()
    reset.add_argument("--reset", dest="reset", action="store_true", default=None,
                       help="reinicia a placa antes de ler (padrão nos papéis ESP32)")
    reset.add_argument("--sem-reset", dest="reset", action="store_false", help="só lê o que a placa escrever")
    parser.add_argument("--timeout-partida", type=float, default=12.0, help="segundos à espera da partida (12)")
    parser.add_argument("--timeout-comando", type=float, default=4.0, help="segundos à espera de cada resposta (4)")
    parser.add_argument("--versao-esperada", default="auto",
                        help="versão que deve estar gravada: 'auto' (arquivo VERSION), um número ou 'nenhuma'")
    parser.add_argument("--commit-esperado", default=None,
                        help="prefixo do commit que deve estar gravado, ou 'atual' (git rev-parse do repositório)")
    parser.add_argument("--estrito", action="store_true", help="trata aviso (commit -dirty ou desconhecido) como falha")
    return parser


def main(argv: Sequence[str] | None = None, *, clock: Callable[[], float] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        plan = build_plan(args.papel, args.no, args.selftest)
    except ValueError as error:
        parser.error(str(error))

    expected_version: str | None
    if args.versao_esperada == "auto":
        expected_version = read_expected_version()
    elif args.versao_esperada == "nenhuma":
        expected_version = None
    else:
        expected_version = args.versao_esperada
    expected_commit = args.commit_esperado
    if expected_commit == "atual":
        expected_commit = current_commit()
        if expected_commit is None:
            parser.error("--commit-esperado atual: não foi possível ler o commit do repositório")

    if args.arquivo:
        text = sys.stdin.read() if args.arquivo == "-" else Path(args.arquivo).read_text(encoding="utf-8", errors="replace")
        lines = [normalize(raw) for raw in text.splitlines()]
        source = args.arquivo
    else:
        port = SerialPort(args.porta, args.baud)
        try:
            lines = collect(plan, port, boot_seconds=args.timeout_partida, command_seconds=args.timeout_comando,
                            reset=args.reset, clock=clock)
        finally:
            port.close()
        source = args.porta
    lines = [line for line in lines if line]
    if not lines:
        print(f"SMOKE RESULT=FAIL ROLE={plan.role} CHECKS=0 WARN=0 FAIL=1 FIRMWARE=- COMMIT=-")
        print(f"{FAIL}: nenhuma linha recebida de {source}. Confira a porta, a velocidade (115200) e o cabo;"
              " se a placa já tinha iniciado, aperte o botão de reiniciar e rode de novo com --sem-reset.",
              file=sys.stderr)
        return 3

    report = evaluate(plan, lines, expected_version=expected_version, expected_commit=expected_commit)
    print(format_report(report, source))
    if report.verdict == "FAIL" or (args.estrito and report.verdict == "WARN"):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
