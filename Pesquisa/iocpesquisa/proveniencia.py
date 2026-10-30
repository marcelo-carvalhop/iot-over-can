"""Proveniência: de onde veio cada número.

Registra a versão do código (commit e se havia alterações não gravadas), as
versões fixadas das ferramentas, os parâmetros de compilação que entram na
análise (lidos das fontes e, quando há, da linha [GW] CONFIG da própria
Probe) e o hash das fontes do firmware. O hash de configuração de uma
execução combina os parâmetros do experimento com os do firmware: dois
resultados com o mesmo hash vieram da mesma configuração.
"""
from __future__ import annotations

import hashlib
import platform
import re
import subprocess
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from iocpesquisa.desenho import sha256_json

FONTES_FIRMWARE = ("Codigo/node-can/src", "Codigo/node-can/include", "Codigo/common/ioc_link",
                   "Codigo/node-wifi", "Codigo/node-can/platformio.ini")


def raiz_repositorio(inicio: Path | None = None) -> Path:
    atual = (inicio or Path(__file__)).resolve()
    for p in [atual, *atual.parents]:
        if (p / "MANIFEST.json").exists() and (p / "Codigo").is_dir():
            return p
    raise FileNotFoundError("raiz do repositório iot-over-can não encontrada")


def _cmd(args: list[str], cwd: Path) -> str:
    try:
        out = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=20, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return out.stdout.strip() if out.returncode == 0 else ""


def git(raiz: Path) -> dict[str, Any]:
    status = _cmd(["git", "status", "--porcelain", "--untracked-files=no"], raiz)
    return {
        "commit": _cmd(["git", "rev-parse", "HEAD"], raiz) or None,
        "ramo": _cmd(["git", "rev-parse", "--abbrev-ref", "HEAD"], raiz) or None,
        "alteracoes_nao_gravadas": bool(status),
    }


def parametros_fonte(raiz: Path) -> dict[str, int]:
    """Valores padrão dos parâmetros de pesquisa, como estão nas fontes."""
    valores: dict[str, int] = {}
    arquivos = [raiz / "Codigo/node-can/include/pesquisa.h", raiz / "Codigo/node-wifi/edge_protocol_definitions.h"]
    padrao = re.compile(r"#define\s+(IOT_[A-Z_]+|NET_[A-Z_]+_MS)\s+(\d+)")
    for arq in arquivos:
        if arq.exists():
            for nome, valor in padrao.findall(arq.read_text(encoding="utf-8", errors="replace")):
                valores[nome] = int(valor)
    return dict(sorted(valores.items()))


def hash_fontes(raiz: Path) -> str:
    h = hashlib.sha256()
    for rel in FONTES_FIRMWARE:
        base = raiz / rel
        arquivos = [base] if base.is_file() else sorted(p for p in base.rglob("*") if p.is_file()
                                                        and p.suffix in {".c", ".h", ".cpp", ".ino", ".ini", ".txt"})
        for arq in arquivos:
            h.update(str(arq.relative_to(raiz)).encode())
            h.update(arq.read_bytes())
    return h.hexdigest()


def fixacoes(raiz: Path) -> dict[str, Any]:
    """Versões fixadas no repositório (plataformas e bibliotecas)."""
    out: dict[str, Any] = {}
    for rel in ("Codigo/node-can/platformio.ini", "Codigo/node-paper/platformio.ini"):
        arq = raiz / rel
        if arq.exists():
            texto = arq.read_text(encoding="utf-8", errors="replace")
            out[rel] = {
                "platform": re.findall(r"^\s*platform\s*=\s*(\S+)", texto, re.MULTILINE),
                "lib_deps": [x.strip() for x in re.findall(r"^\s{2,}(\S+@\S+|\S+/\S+@\S+)\s*$", texto, re.MULTILINE)],
            }
    pico = raiz / "Codigo/node-wifi/CMakeLists.txt"
    if pico.exists():
        out["Codigo/node-wifi"] = {"pico_board_padrao": (re.findall(r"set\(PICO_BOARD (\w+)", pico.read_text()) or [None])[0]}
    return out


def ambiente(raiz: Path | None = None) -> dict[str, Any]:
    raiz = raiz or raiz_repositorio()
    return {
        "registrado_em": datetime.now(UTC).isoformat(timespec="seconds"),
        "python": sys.version.split()[0],
        "plataforma": platform.platform(),
        "git": git(raiz),
        "compiladores": {"cc": _cmd(["cc", "--version"], raiz).splitlines()[:1],
                         "c++": _cmd(["c++", "--version"], raiz).splitlines()[:1]},
        "fixacoes": fixacoes(raiz),
        "parametros_firmware_fonte": parametros_fonte(raiz),
        "fontes_firmware_sha256": hash_fontes(raiz),
    }


def parametros_config(linha: str) -> dict[str, str]:
    """Campos da linha [GW] CONFIG (parâmetros da compilação em execução na Probe)."""
    corpo = linha.split("CONFIG", 1)[1] if "CONFIG" in linha else ""
    campos = dict(re.findall(r"(\w+)=(\S+)", corpo))
    campos.pop("t_us", None)
    return campos


def hash_configuracao(parametros: dict[str, Any], firmware: dict[str, Any], fontes_sha256: str) -> str:
    return sha256_json({"parametros": parametros, "firmware": firmware, "fontes": fontes_sha256})
