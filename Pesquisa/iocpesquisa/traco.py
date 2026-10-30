"""Leitura do registro bruto: linhas da Probe viram eventos tipados.

Cada evento guarda dois relógios: ``t_ref_ms`` (referência da execução) e,
quando a linha o traz, ``t_us`` (relógio da Probe 00). Intervalos entre
eventos da Probe usam ``t_us``; comparações com a verdade de referência
(instante em que o sensor foi desligado) usam ``t_ref_ms``.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Any

KV = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(\S+)")

TIPOS = (
    ("HEARTBEAT", "[GW] HEARTBEAT "),
    ("BUS_STATS", "[GW] BUS_STATS "),
    ("ECHO_DONE", "[GW] ECHO_DONE "),
    ("ECHO_START", "[GW] ECHO_START "),
    ("ECHO", "[GW] ECHO "),
    ("FAULT_EVENT", "[GW] FAULT_EVENT "),
    ("FAULT_TX", "[GW] FAULT_TX "),
    ("ASSOC", "[GW] WIRELESS_ASSOC "),
    ("LINK", "[GW] WIRELESS_LINK "),
    ("STATS", "[GW] WIRELESS_STATS "),
    ("FAILOVER", "[GW] WIRELESS_FAILOVER "),
    ("CONFIG", "[GW] CONFIG "),
    ("BENCH", "[GW] BENCH "),
    ("TEL", "TEL "),
    ("ACOUSTIC", "ACOUSTIC "),
)


@dataclass
class Evento:
    tipo: str
    t_ref_ms: float
    t_us: int | None
    campos: dict[str, str] = field(default_factory=dict)
    linha: str = ""

    def num(self, chave: str, padrao: float | None = None) -> float | None:
        valor = self.campos.get(chave)
        if valor is None:
            return padrao
        try:
            return float(int(valor, 0)) if valor.lower().startswith("0x") else float(valor)
        except ValueError:
            return padrao


def _campos(linha: str) -> dict[str, str]:
    return {k.lower(): v for k, v in KV.findall(linha)}


def evento(registro: dict[str, Any]) -> Evento | None:
    linha = registro.get("linha", "")
    fonte = registro.get("fonte")
    t_ref = float(registro.get("t_ref_ms") or 0.0)
    if fonte in ("bancada", "coletor"):
        if linha.startswith("pesquisa "):
            corpo = linha[len("pesquisa "):]
            return Evento("MARCA", t_ref, None, _campos(corpo), corpo)
        if linha.startswith("comando "):
            return Evento("COMANDO", t_ref, None, {}, linha[len("comando "):])
        return None
    if linha.startswith("BENCH ") or linha.startswith("[NODE ") and " BENCH " in linha:
        corpo = linha[linha.index("BENCH "):]
        c = _campos(corpo)
        return Evento("BENCH", t_ref, None, c, linha)
    for tipo, prefixo in TIPOS:
        if linha.startswith(prefixo):
            c = _campos(linha)
            t_us = c.get("t_us")
            return Evento(tipo, t_ref, int(t_us) if t_us and t_us.isdigit() else None, c, linha)
    return None


def eventos(registros: list[dict[str, Any]]) -> list[Evento]:
    out = []
    for r in registros:
        e = evento(r)
        if e is not None:
            out.append(e)
    return out


def janela(evs: list[Evento]) -> tuple[float, float]:
    """Janela de medição (relógio de referência), pelas marcas INICIO e FIM."""
    inicio = next((e.t_ref_ms for e in evs if e.tipo == "MARCA" and e.campos.get("janela") == "INICIO"), None)
    fim = next((e.t_ref_ms for e in evs if e.tipo == "MARCA" and e.campos.get("janela") == "FIM"), None)
    if inicio is None:
        inicio = evs[0].t_ref_ms if evs else 0.0
    if fim is None:
        fim = evs[-1].t_ref_ms if evs else 0.0
    return inicio, fim
