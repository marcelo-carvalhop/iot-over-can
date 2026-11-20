#!/usr/bin/env python3
"""Compara os tempos de uma gravação da bancada física com os da bancada virtual.

A bancada virtual (Codigo/node-can/test/host/sim_network.cpp) roda o firmware real, mas o rádio, o barramento e
o relógio são modelos. Este script mede, nos dois lados, o que o roteiro de testes anota à mão e coloca lado a
lado, com um critério de aceitação, para dizer se o modelo continua descrevendo a bancada:

    troca_de_lider_ms     do último sinal de presença do líder que saiu ao primeiro do novo líder
    reassociacao_ms       do WIRELESS_FAILOVER até o sensor voltar a ter sessão SECURE no módulo escolhido
    abertura_do_canal_ms  da primeira associação de um sensor até a primeira sessão SECURE dele

A entrada são as linhas que a Probe 00 escreve na serial, com o instante em milissegundos: o formato de
Front/replays/*.log (bancada virtual) e das gravações da estação (``python -m pico_tui --record-session``,
arquivos ``logs/gravacao_*.log``), as duas com ``@<ms> <linha>``. Linhas que começam por ``#`` são ignoradas.

    comparar_bancada.py logs/gravacao_20261117-101500_ab12.log --cenario owner_failover
    comparar_bancada.py gravacao.log --bancada Front/replays/leader_failure.log --tolerancia-relativa 0.4

Critério: uma medida está dentro da tolerância se a diferença for no máximo ``--tolerancia-absoluta-ms``
(1500 por padrão: a latência da USB serial e do próprio laço do firmware) ou ``--tolerancia-relativa`` (0,25)
do valor da bancada virtual, o que for maior. As medidas são comparadas na ordem em que aparecem.

Saída: uma linha por medida e, por último, ``COMPARACAO RESULT=OK|FORA|SEM_DADOS``. Código de saída: 0 todas
dentro da tolerância, 1 alguma fora (ou sem dado na gravação com ``--exigir``), 2 uso incorreto, 3 nenhuma
medida comparável (os dois lados precisam ter o mesmo evento).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

ROOT = Path(__file__).resolve().parents[1]

LINE = re.compile(r"^@(\d+) (.*)$")
HEARTBEAT = re.compile(r"\[GW\] HEARTBEAT lider=(\d+)\b")
FAILOVER = re.compile(r"\[GW\] WIRELESS_FAILOVER uuid=(\S+) from=(\d+) to=(\d+)")
ASSOC = re.compile(
    r"\[GW\] WIRELESS_ASSOC node=(\d+) child=(\d+) uuid=(\S+) profile=\S+ state=(\S+) .*?session=(\S+)"
)

METRICS = ("troca_de_lider_ms", "reassociacao_ms", "abertura_do_canal_ms")

DEFAULT_ABS_MS = 1500
DEFAULT_REL = 0.25


@dataclass(frozen=True)
class Line:
    t_ms: int
    text: str


def read_lines(path: Path) -> list[Line]:
    """As linhas ``@<ms> <texto>`` de uma gravação; o resto (comentários, comandos) é ignorado."""
    lines: list[Line] = []
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = LINE.match(raw)
        if match:
            lines.append(Line(int(match.group(1)), match.group(2)))
    return lines


def leader_changes(lines: Sequence[Line]) -> list[int]:
    """Tempo sem líder em cada troca: do último sinal do líder anterior ao primeiro do seguinte."""
    gaps: list[int] = []
    leader: int | None = None
    last_seen = 0
    for line in lines:
        match = HEARTBEAT.search(line.text)
        if not match:
            continue
        current = int(match.group(1))
        if leader is not None and current != leader:
            gaps.append(line.t_ms - last_seen)
        leader = current
        last_seen = line.t_ms
    return gaps


def reassociations(lines: Sequence[Line]) -> list[int]:
    """Do WIRELESS_FAILOVER até a sessão SECURE do mesmo sensor no módulo escolhido."""
    out: list[int] = []
    pending: list[tuple[str, int, int]] = []   # (uuid, módulo escolhido, instante)
    for line in lines:
        failover = FAILOVER.search(line.text)
        if failover:
            pending.append((failover.group(1), int(failover.group(3)), line.t_ms))
            continue
        assoc = ASSOC.search(line.text)
        if not assoc or assoc.group(5) != "SECURE":
            continue
        node, uuid = int(assoc.group(1)), assoc.group(3)
        for index, (p_uuid, p_node, started) in enumerate(pending):
            if p_uuid == uuid and p_node == node:
                out.append(line.t_ms - started)
                del pending[index]
                break
    return out


def channel_openings(lines: Sequence[Line]) -> list[int]:
    """Da primeira associação de cada sensor à primeira sessão SECURE dele (as reassociações não entram)."""
    out: list[int] = []
    started: dict[str, int] = {}
    done: set[str] = set()
    for line in lines:
        assoc = ASSOC.search(line.text)
        if not assoc:
            continue
        uuid = assoc.group(3)
        if uuid in done:
            continue
        if assoc.group(4) == "ASSOCIATING" and uuid not in started:
            started[uuid] = line.t_ms
        if assoc.group(5) == "SECURE" and uuid in started:
            out.append(line.t_ms - started[uuid])
            done.add(uuid)
    return out


def measure(lines: Sequence[Line]) -> dict[str, list[int]]:
    return {
        "troca_de_lider_ms": leader_changes(lines),
        "reassociacao_ms": reassociations(lines),
        "abertura_do_canal_ms": channel_openings(lines),
    }


@dataclass
class Row:
    metric: str
    index: int
    bench_ms: int | None
    physical_ms: int | None
    limit_ms: int | None
    verdict: str   # OK | FORA | SEM_DADO

    @property
    def delta_ms(self) -> int | None:
        if self.bench_ms is None or self.physical_ms is None:
            return None
        return self.physical_ms - self.bench_ms


@dataclass
class Comparison:
    rows: list[Row] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    @property
    def compared(self) -> list[Row]:
        return [row for row in self.rows if row.verdict != "SEM_DADO"]

    def result(self, require_all: bool) -> str:
        if not self.compared:
            return "SEM_DADOS"
        if any(row.verdict == "FORA" for row in self.rows):
            return "FORA"
        if require_all and any(row.verdict == "SEM_DADO" for row in self.rows):
            return "FORA"
        return "OK"


def tolerance_ms(bench_ms: int, abs_ms: int, rel: float) -> int:
    return int(max(abs_ms, rel * bench_ms))


def compare(
    bench: dict[str, list[int]],
    physical: dict[str, list[int]],
    abs_ms: int = DEFAULT_ABS_MS,
    rel: float = DEFAULT_REL,
) -> Comparison:
    result = Comparison()
    for metric in METRICS:
        b, p = bench.get(metric, []), physical.get(metric, [])
        if not b and not p:
            continue
        if len(b) != len(p):
            result.notes.append(
                f"{metric}: a bancada virtual tem {len(b)} ocorrência(s) e a gravação física, {len(p)}; "
                "comparadas na ordem, só as que existem nos dois lados"
            )
        for index in range(max(len(b), len(p))):
            bench_ms = b[index] if index < len(b) else None
            phys_ms = p[index] if index < len(p) else None
            if bench_ms is None or phys_ms is None:
                result.rows.append(Row(metric, index + 1, bench_ms, phys_ms, None, "SEM_DADO"))
                continue
            limit = tolerance_ms(bench_ms, abs_ms, rel)
            verdict = "OK" if abs(phys_ms - bench_ms) <= limit else "FORA"
            result.rows.append(Row(metric, index + 1, bench_ms, phys_ms, limit, verdict))
    return result


def _fmt(value: int | None) -> str:
    return "-" if value is None else str(value)


def format_report(comparison: Comparison, require_all: bool) -> str:
    out = [f"{'medida':<24}{'n':>3}{'bancada_ms':>12}{'fisica_ms':>11}{'dif_ms':>9}{'limite_ms':>11}  resultado"]
    for row in comparison.rows:
        out.append(
            f"{row.metric:<24}{row.index:>3}{_fmt(row.bench_ms):>12}{_fmt(row.physical_ms):>11}"
            f"{_fmt(row.delta_ms):>9}{_fmt(row.limit_ms):>11}  {row.verdict}"
        )
    out.extend(f"# {note}" for note in comparison.notes)
    outside = sum(1 for row in comparison.rows if row.verdict == "FORA")
    missing = sum(1 for row in comparison.rows if row.verdict == "SEM_DADO")
    out.append(
        f"COMPARACAO RESULT={comparison.result(require_all)} medidas={len(comparison.compared)} "
        f"fora={outside} sem_dado={missing}"
    )
    return "\n".join(out)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("gravacao", type=Path, help="gravação física (linhas '@<ms> <linha>' da Probe 00)")
    origin = parser.add_mutually_exclusive_group()
    origin.add_argument("--cenario", help="cenário da bancada virtual (Front/replays/<cenario>.log)")
    origin.add_argument("--bancada", type=Path, help="gravação da bancada virtual a usar como referência")
    parser.add_argument("--tolerancia-absoluta-ms", type=int, default=DEFAULT_ABS_MS)
    parser.add_argument("--tolerancia-relativa", type=float, default=DEFAULT_REL)
    parser.add_argument("--exigir", action="store_true", help="reprova se a gravação não tiver uma medida que a bancada tem")
    parser.add_argument("--json", action="store_true", help="escreve o resultado em JSON em vez da tabela")
    args = parser.parse_args(argv)

    if args.tolerancia_absoluta_ms < 0 or not 0.0 <= args.tolerancia_relativa <= 10.0:
        parser.error("tolerância fora da faixa")
    if args.bancada is None and not args.cenario:
        parser.error("informe --cenario ou --bancada")
    bench_path = args.bancada or ROOT / "Front" / "replays" / f"{args.cenario}.log"
    for path in (args.gravacao, bench_path):
        if not path.is_file():
            print(f"arquivo não encontrado: {path}", file=sys.stderr)
            return 2

    bench = measure(read_lines(bench_path))
    physical = measure(read_lines(args.gravacao))
    comparison = compare(bench, physical, args.tolerancia_absoluta_ms, args.tolerancia_relativa)
    result = comparison.result(args.exigir)

    if args.json:
        print(json.dumps({
            "result": result,
            "bench": str(bench_path),
            "recording": str(args.gravacao),
            "rows": [row.__dict__ | {"delta_ms": row.delta_ms} for row in comparison.rows],
            "notes": comparison.notes,
        }, ensure_ascii=False, indent=2))
    else:
        print(format_report(comparison, args.exigir))
    if result == "SEM_DADOS":
        print("nenhuma medida existe nos dois lados: confira se a gravação tem o evento (sinal de presença, "
              "WIRELESS_FAILOVER, WIRELESS_ASSOC)", file=sys.stderr)
        return 3
    return 0 if result == "OK" else 1


if __name__ == "__main__":
    sys.exit(main())
