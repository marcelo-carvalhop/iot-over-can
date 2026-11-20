"""A versão do projeto tem uma única fonte, o arquivo VERSION, e todo o resto a deriva dela."""
from __future__ import annotations

import importlib.util
import json
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def versao():
    spec = importlib.util.spec_from_file_location("versao", ROOT / "Codigo" / "scripts" / "versao.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_every_copy_matches_the_version_file_and_no_firmware_hardcodes_it(versao):
    assert versao.check() == []


def test_manifest_and_package_versions_equal_the_version_file(versao):
    version = versao.read_version()
    assert json.loads((ROOT / "MANIFEST.json").read_text(encoding="utf-8"))["version"] == version
    for library in ("ioc_link", "ioc_panel"):
        data = json.loads((ROOT / "Codigo" / "common" / library / "library.json").read_text(encoding="utf-8"))
        assert data["version"] == version


def test_check_catches_a_diverging_copy_and_a_hardcoded_firmware_version(versao, tmp_path, monkeypatch):
    for name, _pattern in versao.COPIES:
        target = tmp_path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text((ROOT / name).read_text(encoding="utf-8"), encoding="utf-8")
    (tmp_path / "VERSION").write_text("9.9.9\n", encoding="utf-8")
    firmware = tmp_path / "Codigo" / "node-can" / "src"
    firmware.mkdir(parents=True)
    (firmware / "main.ino").write_text('Serial.println("[PROBE 00] FIRMWARE=0.21.0 X");\n', encoding="utf-8")
    monkeypatch.setattr(versao, "ROOT", tmp_path)
    monkeypatch.setattr(versao, "VERSION_FILE", tmp_path / "VERSION")
    problems = versao.check()
    assert any(item.startswith("MANIFEST.json") and "9.9.9" in item for item in problems)
    assert any("main.ino:1" in item and "IOT_FW_VERSION" in item for item in problems)
    # --write conserta as cópias (o código do firmware continua sendo problema de quem o escreveu)
    changed = versao.write()
    assert "MANIFEST.json" in changed and not any("MANIFEST" in item for item in versao.check())


def test_version_must_be_x_y_z(versao, tmp_path, monkeypatch):
    monkeypatch.setattr(versao, "VERSION_FILE", tmp_path / "VERSION")
    (tmp_path / "VERSION").write_text("1.2\n", encoding="utf-8")
    with pytest.raises(ValueError):
        versao.read_version()
    with pytest.raises(ValueError):
        versao.write("v1.2.3")


def test_cflags_define_version_and_commit(versao, capsys):
    assert versao.main(["--cflags"]) == 0
    lines = capsys.readouterr().out.strip().splitlines()
    assert lines[0] == f'-DIOT_FW_VERSION="{versao.read_version()}"'
    assert re.fullmatch(r'-DIOT_FW_COMMIT="([0-9a-f]{8}(-dirty)?|unknown)"', lines[1])


def test_commit_is_unknown_outside_a_git_repository(versao, tmp_path):
    assert versao.git_commit(tmp_path) == "unknown"
