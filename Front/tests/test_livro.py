"""Marcação do livro (Documentacao/livro.md) e o script que monta o rascunho."""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "Codigo" / "scripts" / "extrair_livro.py"

ACOUSTIC_CHAPTER = [
    "acustico-visao-geral",
    "acustico-perfis",
    "acustico-hardware",
    "acustico-aquisicao",
    "acustico-firmware",
    "acustico-cadeia-de-medicao",
    "acustico-modos",
    "acustico-configuracao",
    "acustico-transporte",
    "acustico-protocolo",
    "acustico-diagnostico",
    "acustico-tui",
    "acustico-tela-sensor",
    "acustico-build",
    "acustico-validacao",
    "acustico-desafios",
    "acustico-limitacoes",
]


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True, text=True, cwd=ROOT, check=False)


def test_documentation_markers_match_the_book_outline() -> None:
    result = run("--verificar")
    assert result.returncode == 0, result.stderr
    assert "1 capítulo(s) e 17 trecho(s)" in result.stdout


def test_draft_follows_the_outline_and_keeps_links_working(tmp_path: Path) -> None:
    out = tmp_path / "livro.md"
    result = run("-o", str(out))
    assert result.returncode == 0, result.stderr
    text = out.read_text(encoding="utf-8")

    assert text.count("\n# Sensor acústico INMP441\n") == 1
    positions = [text.index(f"<!-- trecho: {ident} | fonte: Documentacao/") for ident in ACOUSTIC_CHAPTER]
    assert positions == sorted(positions)
    assert "livro:inicio" not in text and "livro:fim" not in text
    # Numeração da origem removida, títulos a partir do nível 2, títulos próprios do livro.
    assert "\n## Hardware e ligação\n" in text and "2. Hardware" not in text
    assert "\n### Da palavra I2S à pressão\n" in text
    assert "\n## Perfis de sensor\n" in text
    assert "\n## Desafios e resoluções\n" in text
    # A tabela de desafios chega ao livro com cabeçalho próprio.
    challenges = text[text.index("trecho: acustico-desafios") :]
    assert "| ID | Problema | Causa | Solução aplicada | Estado |" in challenges
    # Links relativos reescritos para o local do rascunho; em [`caminho`](caminho), o texto acompanha.
    for target in re.findall(r"\]\(([^)\s#]+)", text):
        if "://" not in target:
            assert (out.parent / target).exists(), target
    for label, target in re.findall(r"\[`([^`]+)`\]\(([^)\s]+)\)", text):
        if label.endswith(".md"):
            assert label == target


def test_single_chapter_and_stdout(tmp_path: Path) -> None:
    result = run("--capitulo", "Sensor acústico INMP441", "-o", "-")
    assert result.returncode == 0, result.stderr
    assert result.stdout.count("<!-- trecho: ") == len(ACOUSTIC_CHAPTER)
    missing = run("--capitulo", "Capítulo inexistente", "-o", str(tmp_path / "x.md"))
    assert missing.returncode == 1
    assert "não está no roteiro" in missing.stderr
    assert not (tmp_path / "x.md").exists()


def test_verification_reports_broken_markup(tmp_path: Path) -> None:
    docs = tmp_path / "Documentacao"
    docs.mkdir()
    (docs / "a.md").write_text(
        "# A\n\n"
        "| x | y |\n|---|---|\n| 1 | 2 |\n"
        '<!-- livro:inicio id="t-tabela" capitulo="C" -->\n'
        "| 3 | 4 |\n"
        '<!-- livro:fim id="t-tabela" -->\n\n'
        '<!-- livro:inicio id="t-vazio" capitulo="C" -->\n\n'
        '<!-- livro:fim id="t-vazio" -->\n\n'
        "```markdown\n"
        '<!-- livro:inicio id="t-exemplo" capitulo="C" -->\n'
        "```\n\n"
        '<!-- livro:inicio id="t-outro" capitulo="Outro" -->\ntexto\n<!-- livro:fim id="t-outro" -->\n\n'
        '<!-- livro:inicio id="Errado" capitulo="C" -->\ntexto\n<!-- livro:fim id="Errado" -->\n\n'
        "<!-- livro:inicio t-sem-id -->\n\n"
        '<!-- livro:inicio id="t-aberto" capitulo="C" -->\ntexto sem fim\n',
        encoding="utf-8",
    )
    (docs / "b.md").write_text(
        '<!-- livro:inicio id="t-tabela" capitulo="C" -->\nrepetido\n<!-- livro:fim id="t-tabela" -->\n'
        '<!-- livro:fim id="t-solto" -->\n',
        encoding="utf-8",
    )
    (docs / "livro.md").write_text(
        "# Livro\n\n## Roteiro\n\n### C\n\n"
        "| Ordem | Trecho | Arquivo | Conteúdo |\n|---|---|---|---|\n"
        "| 1 | `t-tabela` | [`b.md`](b.md) | arquivo errado |\n"
        "| 2 | `t-vazio` | [`a.md`](a.md) | vazio |\n"
        "| 3 | `t-outro` | [`a.md`](a.md) | capítulo divergente |\n"
        "| 5 | `t-fantasma` | [`a.md`](a.md) | não marcado |\n",
        encoding="utf-8",
    )
    result = run("--verificar", "--docs", str(docs))
    assert result.returncode == 1
    err = result.stderr
    assert "a.md:6: marcador dentro de tabela" in err
    assert err.count("dentro de tabela") == 1  # o fim, depois da última linha, não quebra nada
    assert "'t-vazio' está vazio" in err
    assert "t-exemplo" not in err  # dentro de bloco de código é só exemplo
    assert "'t-outro' declara o capítulo 'Outro', mas o roteiro o põe em 'C'" in err
    assert "id inválido 'Errado'" in err
    assert "marcador do livro malformado: <!-- livro:inicio t-sem-id -->" in err
    assert "'t-aberto' não foi fechado" in err
    assert "id 't-tabela' repetido (já usado em a.md:6)" in err
    assert "fim de 't-solto' sem início" in err
    assert "'t-tabela' está em a.md, não em b.md" in err
    assert "ordem 5 fora de sequência" in err
    assert "'t-fantasma' não está marcado em nenhum documento" in err
    assert "o trecho 'Errado' não está no roteiro" in err
