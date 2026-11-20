#!/usr/bin/env python3
"""Versão única do projeto: o arquivo VERSION, na raiz do repositório.

Tudo o que mostra a versão a alguém a deriva daqui:

* firmware: ``IOT_FW_VERSION`` e ``IOT_FW_COMMIT`` entram na compilação (``--cflags``,
  ``pio_versao.py`` e ``ioc_version.cmake``) e saem no boot e no comando ``VERSION``;
* pacotes e manifestos que precisam de um número escrito no próprio arquivo
  (``MANIFEST.json``, ``Front/pyproject.toml``, ``Front/pico_tui/__init__.py``,
  ``Pesquisa/iocpesquisa/__init__.py`` e os ``library.json``): ``--write`` os atualiza e
  ``--check`` (usado no CI) falha se algum divergir ou se um firmware voltar a ter a
  versão escrita no código.

Uso:
    versao.py                  mostra a versão
    versao.py --commit         commit do git (com -dirty se houver alterações)
    versao.py --check          confere as cópias e o código dos firmwares (código de saída 1 se divergir)
    versao.py --write [X.Y.Z]  grava a versão (a do VERSION, ou a nova) em todas as cópias
    versao.py --cflags         definições -D para compilar o firmware, uma por linha
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERSION_FILE = ROOT / "VERSION"
SEMVER = re.compile(r"^\d+\.\d+\.\d+$")

# (arquivo, expressão cujo grupo 2 é o número). O grupo 1 e o 3 são mantidos.
COPIES: tuple[tuple[str, str], ...] = (
    ("MANIFEST.json", r'("version":\s*")([^"]+)(")'),
    ("Front/pyproject.toml", r'(^version\s*=\s*")([^"]+)(")'),
    ("Front/pico_tui/__init__.py", r'(__version__\s*=\s*")([^"]+)(")'),
    ("Pesquisa/iocpesquisa/__init__.py", r'(__version__\s*=\s*")([^"]+)(")'),
    ("Codigo/common/ioc_link/library.json", r'("version":\s*")([^"]+)(")'),
    ("Codigo/common/ioc_panel/library.json", r'("version":\s*")([^"]+)(")'),
)

# Firmware com a versão escrita no código: deve usar IOT_FW_VERSION.
HARDCODED = re.compile(r'(FIRMWARE=|FW_VERSION\s+|fw=|firmware\s+)"?\d+\.\d+\.\d+')
FIRMWARE_SOURCES = ("Codigo/node-can/src", "Codigo/node-can/include", "Codigo/node-wifi", "Codigo/node-audio",
                    "Codigo/node-paper/src", "Codigo/leitor/src", "Codigo/common/ioc_link", "Codigo/common/ioc_panel")


def read_version() -> str:
    text = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not SEMVER.match(text):
        raise ValueError(f"VERSION inválido: {text!r}. Use X.Y.Z, por exemplo 0.22.0.")
    return text


def git_commit(root: Path = ROOT) -> str:
    """Commit curto do git, com ``-dirty`` se houver alterações em arquivos versionados; ``unknown`` fora do git."""

    def run(*args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, check=False)

    try:
        head = run("rev-parse", "--short=8", "HEAD")
        if head.returncode != 0 or not head.stdout.strip():
            return "unknown"
        dirty = run("status", "--porcelain", "--untracked-files=no").stdout.strip()
    except OSError:  # sem o git instalado
        return "unknown"
    return head.stdout.strip() + ("-dirty" if dirty else "")


def defines() -> list[tuple[str, str]]:
    """Pares (nome, valor sem aspas) para o compilador."""

    return [("IOT_FW_VERSION", read_version()), ("IOT_FW_COMMIT", git_commit())]


def check() -> list[str]:
    """Lista o que diverge da versão do VERSION (vazia se tudo confere)."""

    problems: list[str] = []
    try:
        version = read_version()
    except (OSError, ValueError) as exc:
        return [str(exc)]
    for name, pattern in COPIES:
        path = ROOT / name
        if not path.exists():
            problems.append(f"{name}: arquivo não encontrado")
            continue
        match = re.search(pattern, path.read_text(encoding="utf-8"), re.MULTILINE)
        if match is None:
            problems.append(f"{name}: não encontrei o número da versão")
        elif match.group(2) != version:
            problems.append(f"{name}: {match.group(2)} (o VERSION diz {version})")
    for folder in FIRMWARE_SOURCES:
        for path in sorted((ROOT / folder).rglob("*")):
            if path.suffix not in {".c", ".cpp", ".h", ".ino"} or "test" in path.parts or "build" in path.parts:
                continue
            for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                if HARDCODED.search(line) and "IOT_FW_VERSION" not in line:
                    problems.append(f"{path.relative_to(ROOT)}:{number}: versão escrita no código; use IOT_FW_VERSION")
    return problems


def write(version: str | None = None) -> list[str]:
    """Grava a versão em todas as cópias. Devolve os arquivos alterados."""

    if version is not None:
        if not SEMVER.match(version):
            raise ValueError(f"Versão inválida: {version!r}. Use X.Y.Z.")
        VERSION_FILE.write_text(version + "\n", encoding="utf-8")
    version = read_version()
    changed: list[str] = []
    for name, pattern in COPIES:
        path = ROOT / name
        text = path.read_text(encoding="utf-8")
        new = re.sub(pattern, lambda m: f"{m.group(1)}{version}{m.group(3)}", text, count=1, flags=re.MULTILINE)
        if new != text:
            path.write_text(new, encoding="utf-8")
            changed.append(name)
    return changed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--commit", action="store_true", help="mostra o commit do git")
    group.add_argument("--check", action="store_true", help="confere as cópias e o código dos firmwares")
    group.add_argument("--write", nargs="?", const="", metavar="X.Y.Z", help="grava a versão em todas as cópias")
    group.add_argument("--cflags", action="store_true", help="definições -D para o compilador, uma por linha")
    args = parser.parse_args(argv)
    try:
        if args.commit:
            print(git_commit())
        elif args.check:
            problems = check()
            for problem in problems:
                print(f"DIVERGE: {problem}", file=sys.stderr)
            if problems:
                return 1
            print(f"Versão {read_version()}: todas as cópias conferem e nenhum firmware a escreve no código.")
        elif args.write is not None:
            for name in write(args.write or None):
                print(f"atualizado: {name}")
            print(f"Versão {read_version()} gravada.")
        elif args.cflags:
            for name, value in defines():
                print(f'-D{name}="{value}"')
        else:
            print(read_version())
    except (OSError, ValueError) as exc:
        print(f"ERRO: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
