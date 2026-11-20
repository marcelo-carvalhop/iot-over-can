"""run_tui.sh repassa à estação o que o operador configura, sem exigir que chame o programa direto."""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def fake_repo(tmp_path):
    """Cópia do script numa árvore mínima, com um ``iot-over-can-tui`` que só imprime os argumentos."""

    scripts = tmp_path / "Codigo" / "scripts"
    scripts.mkdir(parents=True)
    shutil.copy(ROOT / "Codigo" / "scripts" / "run_tui.sh", scripts / "run_tui.sh")
    shutil.copy(ROOT / "Codigo" / "scripts" / "operadores.sh", scripts / "operadores.sh")
    venv_bin = tmp_path / "Front" / ".venv" / "bin"
    venv_bin.mkdir(parents=True)
    (venv_bin / "activate").write_text(f'export PATH="{venv_bin}:$PATH"\n')
    tui = venv_bin / "iot-over-can-tui"
    tui.write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$@"\n')
    tui.chmod(0o755)
    # python do "ambiente virtual": mostra o que o operadores.sh pediria à CLI.
    python = venv_bin / "python"
    python.write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$@"\n')
    python.chmod(0o755)
    return tmp_path


def run_script(fake_repo, name, *args, **env):
    environment = {key: value for key, value in os.environ.items()
                   if key not in {"SECURITY_MODE", "OPERATOR_MODE", "OPERATOR_IDLE", "OPERATORS_FILE"}}
    environment.update(env)
    result = subprocess.run(["bash", str(fake_repo / "Codigo" / "scripts" / name), *args],
                            capture_output=True, text=True, env=environment, check=True)
    return result.stdout.split()


def test_defaults_are_unchanged(fake_repo):
    assert run_script(fake_repo, "run_tui.sh", "/dev/ttyUSB0") == [
        "--mode", "auto", "--security-mode", "presence", "--operator-mode", "auto", "--port", "/dev/ttyUSB0"]
    assert run_script(fake_repo, "run_tui.sh") == [
        "--mode", "auto", "--security-mode", "presence", "--operator-mode", "auto"]


def test_idle_time_and_registry_file_are_forwarded(fake_repo):
    args = run_script(fake_repo, "run_tui.sh", "/dev/ttyUSB0", OPERATOR_IDLE="1", OPERATORS_FILE="/tmp/cadastro.json",
                      SECURITY_MODE="off", OPERATOR_MODE="required")
    assert args[args.index("--operator-idle") + 1] == "1"
    assert args[args.index("--operators-file") + 1] == "/tmp/cadastro.json"
    assert args[args.index("--security-mode") + 1] == "off" and args[args.index("--operator-mode") + 1] == "required"
    assert run_script(fake_repo, "run_tui.sh", OPERATOR_IDLE="0").count("--operator-idle") == 1  # 0 também vale


def test_extra_arguments_after_the_port_reach_the_station(fake_repo):
    args = run_script(fake_repo, "run_tui.sh", "/dev/ttyUSB0", "--operator-idle", "5", "--ascii")
    assert args[-3:] == ["--operator-idle", "5", "--ascii"] and "--port" in args


def test_operators_script_uses_the_same_registry_file(fake_repo):
    assert "--file" not in run_script(fake_repo, "operadores.sh", "list")
    args = run_script(fake_repo, "operadores.sh", "list", OPERATORS_FILE="/tmp/cadastro.json")
    assert args[args.index("--file") + 1] == "/tmp/cadastro.json" and args[-1] == "list"
