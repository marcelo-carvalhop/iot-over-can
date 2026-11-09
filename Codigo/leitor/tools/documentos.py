#!/usr/bin/env python3
"""Converte a documentação do repositório no formato do leitor.

O leitor (M5Stack PaperColor) não interpreta Markdown: este script faz isso na
compilação e gera ``src/documentos.h``, com o texto de cada documento já
separado em blocos. Cada linha do texto gerado começa com um código de bloco:

    1 título do documento    2 seção          3 subseção       4 tópico
    P parágrafo              - item de lista  = item aninhado  # item numerado
    C linha de código        Q citação        I imagem
    T título de cartão (primeira coluna de uma linha de tabela)
    t detalhe de cartão ("Coluna: valor")
    D definição (tabela de duas colunas: "termo: significado")

Dentro do texto, três bytes de controle alternam estilos: 0x01 negrito,
0x02 código e 0x03 itálico. Os caracteres que a fonte da tela não tem viram
equivalentes simples (traços, aspas, setas, desenhos de caixa); o que sobrar é
avisado na compilação.

Uso: documentos.py [--saida src/documentos.h] [--raiz <repositório>] [--texto <pasta>]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

NEGRITO, CODIGO, ITALICO = "\x01", "\x02", "\x03"

# Ordem da biblioteca: arquivo, título curto e resumo de uma linha.
DOCUMENTOS = [
    ("README.md", "Apresentação", "o que é o projeto e como as peças se ligam"),
    ("Documentacao/guia-de-uso.md", "Guia de uso", "montar, gravar, ligar e operar a rede"),
    ("Documentacao/arquitetura.md", "Arquitetura", "como o sistema funciona e por quê"),
    ("Documentacao/protocolo.md", "Protocolo", "identificadores CAN e formatos das mensagens"),
    ("Documentacao/integracao.md", "Integração", "como estender o projeto ou fazer um fork"),
    ("Documentacao/tui.md", "TUI", "telas, teclas e arquivos da estação"),
    ("Documentacao/sensor-acustico.md", "Sensor acústico", "hardware, medição, modos e análises"),
    ("Documentacao/seguranca.md", "Segurança", "chaves, autorização, ameaças e limites"),
    ("Documentacao/validacao.md", "Validação", "testes, bancada virtual e bancada física"),
    ("Documentacao/desafios.md", "Desafios", "problemas encontrados e como foram resolvidos"),
]

# Caracteres fora do Latin-1, ou que a fonte desenha com largura dupla.
TROCAS = {
    "–": "-", "—": "-", "‑": "-", "−": "-", "‐": "-",
    "…": "...", "“": '"', "”": '"', "„": '"', "‘": "'", "’": "'",
    "•": "-", "→": "->", "←": "<-", "↔": "<->", "⇒": "=>", "⇄": "<->",
    "≥": ">=", "≤": "<=", "≠": "!=", "≈": "~", "∞": "inf",
    "✓": "v", "✔": "v", "✗": "x", "✘": "x", "⚠": "!",
    "▲": "^", "▼": "v", "▶": ">", "◀": "<", "►": ">", "◄": "<",
    "●": "*", "○": "o", "■": "#", "□": "[]", "↑": "^", "↓": "v",
    " ": " ", " ": " ", " ": " ", " ": " ", " ": " ", "​": "",
    "′": "'", "″": '"', "­": "",
}

# Símbolos das fórmulas: a fonte não tem o alfabeto grego nem sobrescritos além de ¹, ² e ³.
TROCAS.update({"Ω": "ohm", "Δ": "d", "τ": "tau", "√": "raiz", "⁸": "^8"})

def _caixa(ch: str) -> str | None:
    """Desenho de caixa (U+2500 a U+257F) em ASCII, preservando o alinhamento."""
    cp = ord(ch)
    if not 0x2500 <= cp <= 0x257F:
        return None
    if ch in "═╍╌┅┄━─╴╶╼╾":
        return "=" if ch == "═" else "-"
    if ch in "║┃│╎╏┆┇┊┋╵╷╽╿":
        return "|"
    return "+"


@dataclass
class Conversao:
    avisos: dict = field(default_factory=dict)

    def caracteres(self, texto: str) -> str:
        out = []
        for ch in texto:
            if ord(ch) < 0x80 or (0xA0 <= ord(ch) <= 0xFF and ch not in TROCAS):
                out.append(ch)
                continue
            if ch in TROCAS:
                out.append(TROCAS[ch])
                continue
            caixa = _caixa(ch)
            if caixa is not None:
                out.append(caixa)
                continue
            self.avisos[ch] = self.avisos.get(ch, 0) + 1
            out.append("?")
        return "".join(out)

    # ------------------------------------------------------------ em linha

    def em_linha(self, texto: str) -> str:
        """Markdown em linha para o texto do leitor (estilos em bytes de controle)."""
        partes = re.split(r"(`[^`]*`)", texto)
        out = []
        for parte in partes:
            if len(parte) >= 2 and parte.startswith("`") and parte.endswith("`"):
                out.append(CODIGO + self.caracteres(parte[1:-1]) + CODIGO)
                continue
            p = re.sub(r"<!--.*?-->", "", parte)
            p = re.sub(r"<br\s*/?>", " ", p)
            p = re.sub(r"!\[([^\]]*)\]\([^)]*\)", lambda m: f"[imagem: {m.group(1)}]", p)
            p = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", p)
            p = re.sub(r"<(https?://[^>]+)>", r"\1", p)
            p = p.replace("&nbsp;", " ").replace("&#160;", " ").replace("&amp;", "&")
            p = p.replace("&lt;", "<").replace("&gt;", ">")
            p = re.sub(r"\\([\\`*_{}\[\]()#+\-.!|>])", r"\1", p)
            p = re.sub(r"\*\*(.+?)\*\*", NEGRITO + r"\1" + NEGRITO, p)
            p = re.sub(r"__(.+?)__", NEGRITO + r"\1" + NEGRITO, p)
            p = re.sub(r"(?<![\w*])\*(?!\s)([^*\n]+?)(?<!\s)\*(?![\w*])", ITALICO + r"\1" + ITALICO, p)
            out.append(self.caracteres(p))
        return re.sub(r"[ \t]+", " ", "".join(out)).strip()

    # ------------------------------------------------------------- blocos

    def documento(self, markdown: str) -> list[str]:
        linhas = markdown.replace("\r\n", "\n").replace("\t", "    ").split("\n")
        blocos: list[str] = []
        i = 0
        paragrafo: list[str] = []
        item: list[str] | None = None  # [código do bloco, texto]

        def fecha() -> None:
            nonlocal paragrafo, item
            if paragrafo:
                blocos.append("P" + self.em_linha(" ".join(paragrafo)))
                paragrafo = []
            if item is not None:
                blocos.append(item[0] + self.em_linha(item[1]))
                item = None

        while i < len(linhas):
            bruta = linhas[i]
            linha = bruta.strip()
            recuo = len(bruta) - len(bruta.lstrip(" "))

            if linha.startswith("<!--"):
                fecha()
                while i < len(linhas) and "-->" not in linhas[i]:
                    i += 1
                i += 1
                continue
            if linha.startswith("```"):
                fecha()
                i += 1
                while i < len(linhas) and not linhas[i].strip().startswith("```"):
                    codigo = linhas[i][recuo:] if linhas[i][:recuo].strip() == "" else linhas[i].lstrip()
                    blocos.append("C" + self.caracteres(codigo.rstrip()))
                    i += 1
                i += 1
                continue
            if not linha:
                fecha()
                i += 1
                continue
            titulo = re.match(r"^(#{1,6})\s+(.*?)\s*#*$", linha)
            if titulo and recuo < 4:
                fecha()
                nivel = min(len(titulo.group(1)), 4)
                blocos.append(str(nivel) + self.em_linha(titulo.group(2)))
                i += 1
                continue
            if linha.startswith("|"):
                fecha()
                tabela = []
                while i < len(linhas) and linhas[i].strip().startswith("|"):
                    tabela.append(linhas[i].strip())
                    i += 1
                blocos.extend(self.tabela(tabela))
                continue
            imagem = re.match(r"^!\[([^\]]*)\]\([^)]*\)$", linha)
            if imagem:
                fecha()
                blocos.append("I" + self.em_linha(f"[imagem: {imagem.group(1)}]"))
                i += 1
                continue
            if re.match(r"^(-{3,}|\*{3,}|_{3,})$", linha):
                fecha()
                i += 1
                continue
            lista = re.match(r"^([-*+]|\d+[.)])\s+(.*)$", linha)
            if lista:
                fecha()
                if lista.group(1)[0].isdigit():
                    item = ["#", lista.group(1) + " " + lista.group(2)]
                else:
                    item = ["=" if recuo >= 2 else "-", lista.group(2)]
                i += 1
                continue
            if linha.startswith(">"):
                fecha()
                citacao = []
                while i < len(linhas) and linhas[i].strip().startswith(">"):
                    citacao.append(linhas[i].strip()[1:].strip())
                    i += 1
                blocos.append("Q" + self.em_linha(" ".join(citacao)))
                continue
            if item is not None:
                item[1] += " " + linha
            else:
                paragrafo.append(linha)
            i += 1
        fecha()
        return [b for b in blocos if len(b) > 1 or b == "C"]

    def tabela(self, linhas: list[str]) -> list[str]:
        """Tabela em cartões: na tela estreita, uma grade com várias colunas não cabe."""

        def celulas(linha: str) -> list[str]:
            corpo = linha.strip()
            corpo = corpo[1:] if corpo.startswith("|") else corpo
            corpo = corpo[:-1] if corpo.endswith("|") and not corpo.endswith("\\|") else corpo
            return [c.strip() for c in re.split(r"(?<!\\)\|", corpo)]

        def separador(linha: str) -> bool:
            return "-" in linha and not linha.replace("|", "").replace(":", "").replace("-", "").strip()

        linhas = [l for l in linhas if not separador(l)]
        if not linhas:
            return []
        cabecalho = [self.em_linha(c) for c in celulas(linhas[0])]
        saida: list[str] = []
        if len(cabecalho) == 2:
            saida.append("D" + ITALICO + f"{cabecalho[0]}: {cabecalho[1]}" + ITALICO)
            for linha in linhas[1:]:
                c = [self.em_linha(x) for x in celulas(linha)] + ["", ""]
                saida.append("D" + NEGRITO + c[0] + NEGRITO + (": " + c[1] if c[1] else ""))
            return saida
        for linha in linhas[1:]:
            c = [self.em_linha(x) for x in celulas(linha)]
            if not c or not any(c):
                continue
            saida.append("T" + c[0])
            for nome, valor in zip(cabecalho[1:], c[1:]):
                if valor:
                    saida.append("t" + NEGRITO + nome + ":" + NEGRITO + " " + valor)
        return saida


# --------------------------------------------------------------- C++

def literal(texto: str, largura: int = 100) -> str:
    """Literal C++ em pedaços, com escapes seguros (octais de três dígitos)."""
    pedacos, atual = [], []
    tamanho = 0
    for ch in texto:
        if ch == "\\":
            e = "\\\\"
        elif ch == '"':
            e = '\\"'
        elif ch == "?":
            e = "\\?"
        elif ch == "\n":
            e = "\\n"
        elif ord(ch) < 0x20:
            e = "\\%03o" % ord(ch)
        else:
            e = ch
        atual.append(e)
        tamanho += 1
        if tamanho >= largura or ch == "\n":
            pedacos.append('"' + "".join(atual) + '"')
            atual, tamanho = [], 0
    if atual or not pedacos:
        pedacos.append('"' + "".join(atual) + '"')
    return "\n    ".join(pedacos)


def versao(raiz: Path) -> str:
    try:
        saida = subprocess.run(
            ["git", "-C", str(raiz), "log", "-1", "--format=%h, %cs", "--", "README.md", "Documentacao"],
            capture_output=True, text=True, timeout=10, check=True,
        ).stdout.strip()
        if saida:
            return saida
    except (OSError, subprocess.SubprocessError):
        pass
    return "versão local"


def converter(raiz: Path) -> tuple[list[tuple[str, str, str]], Conversao]:
    conv = Conversao()
    docs = []
    for caminho, titulo, resumo in DOCUMENTOS:
        texto = "\n".join(conv.documento((raiz / caminho).read_text(encoding="utf-8")))
        docs.append((conv.caracteres(titulo), conv.caracteres(resumo), texto))
    return docs, conv


def cabecalho(docs: list[tuple[str, str, str]], ver: str) -> str:
    partes = [
        "// Gerado por Codigo/leitor/tools/documentos.py a partir da documentação do",
        "// repositório. Não edite: compile de novo para atualizar.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "namespace leitor {",
        "",
        "struct Documento {",
        "  const char* titulo;",
        "  const char* resumo;",
        "  const char* texto;",
        "  uint32_t tamanho;",
        "};",
        "",
        f"static const char DOCS_VERSAO[] = {literal(ver)};",
        "",
    ]
    for n, (_, _, texto) in enumerate(docs):
        partes.append(f"static const char DOC_{n}[] =\n    {literal(texto)};")
        partes.append("")
    partes.append("static const Documento DOCUMENTOS[] = {")
    for n, (titulo, resumo, _) in enumerate(docs):
        partes.append(f"  {{{literal(titulo)}, {literal(resumo)}, DOC_{n}, sizeof(DOC_{n}) - 1}},")
    partes.append("};")
    partes.append(f"static const uint8_t DOCUMENTOS_QTD = {len(docs)};")
    partes.append("")
    partes.append("}  // namespace leitor")
    partes.append("")
    return "\n".join(partes)


def gerar(saida: Path, raiz: Path, pasta_texto: Path | None = None, silencioso: bool = False,
          avisos: dict[str, int] | None = None) -> int:
    docs, conv = converter(raiz)
    if avisos is not None:
        avisos.update(conv.avisos)
    conteudo = cabecalho(docs, versao(raiz))
    if not saida.exists() or saida.read_text(encoding="utf-8") != conteudo:
        saida.parent.mkdir(parents=True, exist_ok=True)
        saida.write_text(conteudo, encoding="utf-8")
    if pasta_texto:
        pasta_texto.mkdir(parents=True, exist_ok=True)
        for n, (titulo, _, texto) in enumerate(docs):
            (pasta_texto / f"{n:02d}.txt").write_text(texto, encoding="utf-8")
    total = sum(len(t.encode("utf-8")) for _, _, t in docs)
    if not silencioso:
        print(f"leitor: {len(docs)} documentos, {total // 1024} KB de texto -> {saida}")
    for ch, n in sorted(conv.avisos.items()):
        print(f"leitor: aviso: caractere U+{ord(ch):04X} ({ch}) sem equivalente na fonte, {n} vez(es)", file=sys.stderr)
    return total


def main() -> None:
    aqui = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="Gera o cabeçalho com a documentação do leitor")
    ap.add_argument("--saida", type=Path, default=aqui.parent / "src" / "documentos.h")
    ap.add_argument("--raiz", type=Path, default=aqui.parents[2])
    ap.add_argument("--texto", type=Path, help="grava também o texto convertido de cada documento")
    ap.add_argument("--silencioso", action="store_true", help="não mostra o resumo")
    ap.add_argument("--estrito", action="store_true", help="falha se algum caractere não existir na fonte do leitor")
    args = ap.parse_args()
    avisos: dict[str, int] = {}
    gerar(args.saida, args.raiz, args.texto, args.silencioso, avisos)
    if args.estrito and avisos:
        sys.exit("leitor: acrescente os caracteres acima a TROCAS em tools/documentos.py")


if __name__ == "__main__":
    main()
