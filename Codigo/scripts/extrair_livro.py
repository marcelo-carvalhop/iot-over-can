#!/usr/bin/env python3
"""Reúne os trechos da documentação marcados para o livro do projeto.

Os trechos ficam nos próprios documentos de Documentacao/, entre os marcadores

    <!-- livro:inicio id="acustico-hardware" capitulo="Sensor acústico INMP441" -->
    <!-- livro:fim id="acustico-hardware" -->

(o início aceita ainda titulo="...", que substitui no livro o primeiro título do
trecho) e o roteiro (capítulos e ordem dos trechos) está em Documentacao/livro.md.

Uso:
    extrair_livro.py --verificar                    confere a marcação e o roteiro
    extrair_livro.py [-o SAÍDA] [--capitulo NOME]   gera o rascunho (padrão: build/livro/livro.md;
                                                    "-o -" escreve na saída padrão)

Sai com código 1, listando os problemas, quando a marcação e o roteiro não batem.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from urllib.parse import quote, unquote

REPO_ROOT = Path(__file__).resolve().parents[2]
ROTEIRO = "livro.md"

INICIO = re.compile(r'^<!--\s*livro:inicio\s+id="([^"]*)"\s+capitulo="([^"]*)"(?:\s+titulo="([^"]*)")?\s*-->\s*$')
FIM = re.compile(r'^<!--\s*livro:fim\s+id="([^"]*)"\s*-->\s*$')
ID_VALIDO = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
CERCA = re.compile(r"^\s*(```|~~~)")
TITULO = re.compile(r"^(#{1,6})\s+(.*)$")
NUMERACAO = re.compile(r"^\d+(?:\.\d+)*\.?\s+")
LINK = re.compile(r'(\]\()([^)\s]+)((?:\s+"[^"]*")?\))')
ESQUEMA = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")
CAPITULO_ROTEIRO = re.compile(r"^###\s+(.+?)\s*$")
LINHA_ROTEIRO = re.compile(r"^\|\s*(\d+)\s*\|\s*`([^`]+)`\s*\|\s*\[`([^`]+)`\]\(([^)]+)\)\s*\|")


@dataclass
class Trecho:
    id: str
    capitulo: str
    arquivo: str  # relativo à pasta da documentação
    linha: int  # linha do marcador de início
    titulo: str | None = None  # substitui o primeiro título do trecho no livro
    linhas: list[str] = field(default_factory=list)


@dataclass
class ItemRoteiro:
    id: str
    arquivo: str
    linha: int


@dataclass
class Capitulo:
    nome: str
    itens: list[ItemRoteiro] = field(default_factory=list)


def _tabela(linhas: list[str], indice: int) -> bool:
    """O marcador em ``linhas[indice]`` está entre duas linhas de tabela?"""

    antes = linhas[indice - 1].lstrip() if indice > 0 else ""
    depois = linhas[indice + 1].lstrip() if indice + 1 < len(linhas) else ""
    return antes.startswith("|") and depois.startswith("|")


def ler_trechos(docs: Path, problemas: list[str]) -> dict[str, Trecho]:
    trechos: dict[str, Trecho] = {}
    for caminho in sorted(docs.rglob("*.md")):
        relativo = caminho.relative_to(docs).as_posix()
        if relativo == ROTEIRO:
            continue
        linhas = caminho.read_text(encoding="utf-8").splitlines()
        aberto: Trecho | None = None
        em_codigo = False
        for indice, linha in enumerate(linhas):
            local = f"{relativo}:{indice + 1}"
            if CERCA.match(linha):
                em_codigo = not em_codigo
            elif not em_codigo and linha.lstrip().startswith("<!--") and "livro:" in linha:
                inicio = INICIO.match(linha)
                fim = FIM.match(linha)
                if _tabela(linhas, indice):
                    problemas.append(f"{local}: marcador dentro de tabela (quebra a tabela; ponha-o antes do cabeçalho)")
                if inicio:
                    ident, capitulo, titulo = inicio.groups()
                    if not ID_VALIDO.match(ident):
                        problemas.append(f"{local}: id inválido '{ident}' (use minúsculas, dígitos e hífens)")
                    if titulo is not None and not titulo.strip():
                        problemas.append(f"{local}: titulo vazio em '{ident}'")
                    if aberto is not None:
                        problemas.append(f"{local}: '{ident}' começa antes do fim de '{aberto.id}' (aberto na linha {aberto.linha})")
                    aberto = Trecho(ident, capitulo, relativo, indice + 1, titulo)
                elif fim:
                    ident = fim.group(1)
                    if aberto is None:
                        problemas.append(f"{local}: fim de '{ident}' sem início")
                    elif ident != aberto.id:
                        problemas.append(f"{local}: fim de '{ident}', mas o trecho aberto é '{aberto.id}'")
                    else:
                        if not any(texto.strip() for texto in aberto.linhas):
                            problemas.append(f"{relativo}:{aberto.linha}: o trecho '{ident}' está vazio")
                        anterior = trechos.get(ident)
                        if anterior is not None:
                            problemas.append(
                                f"{relativo}:{aberto.linha}: id '{ident}' repetido (já usado em {anterior.arquivo}:{anterior.linha})"
                            )
                        else:
                            trechos[ident] = aberto
                        aberto = None
                else:
                    problemas.append(f"{local}: marcador do livro malformado: {linha.strip()}")
                continue
            if aberto is not None:
                aberto.linhas.append(linha)
        if aberto is not None:
            problemas.append(f"{relativo}:{aberto.linha}: o trecho '{aberto.id}' não foi fechado")
    return trechos


def ler_roteiro(docs: Path, problemas: list[str]) -> list[Capitulo]:
    caminho = docs / ROTEIRO
    if not caminho.is_file():
        problemas.append(f"{ROTEIRO}: roteiro não encontrado em {docs}")
        return []
    capitulos: list[Capitulo] = []
    em_roteiro = False
    em_codigo = False
    for numero, linha in enumerate(caminho.read_text(encoding="utf-8").splitlines(), start=1):
        if CERCA.match(linha):
            em_codigo = not em_codigo
            continue
        if em_codigo:
            continue
        if linha.startswith("## "):
            em_roteiro = linha[3:].strip() == "Roteiro"
            continue
        if not em_roteiro:
            continue
        capitulo = CAPITULO_ROTEIRO.match(linha)
        if capitulo:
            capitulos.append(Capitulo(capitulo.group(1)))
            continue
        item = LINHA_ROTEIRO.match(linha)
        if not item:
            continue
        ordem, ident, arquivo, destino = item.groups()
        local = f"{ROTEIRO}:{numero}"
        if not capitulos:
            problemas.append(f"{local}: trecho '{ident}' fora de capítulo")
            continue
        if destino != arquivo:
            problemas.append(f"{local}: o link de '{ident}' aponta para {destino}, mas o arquivo indicado é {arquivo}")
        if int(ordem) != len(capitulos[-1].itens) + 1:
            problemas.append(f"{local}: ordem {ordem} fora de sequência no capítulo '{capitulos[-1].nome}'")
        capitulos[-1].itens.append(ItemRoteiro(ident, arquivo, numero))
    if not capitulos:
        problemas.append(f"{ROTEIRO}: nenhum capítulo na seção '## Roteiro'")
    return capitulos


def conferir(capitulos: list[Capitulo], trechos: dict[str, Trecho], problemas: list[str]) -> None:
    listados: dict[str, str] = {}
    nomes: set[str] = set()
    for capitulo in capitulos:
        if capitulo.nome in nomes:
            problemas.append(f"{ROTEIRO}: capítulo '{capitulo.nome}' repetido")
        nomes.add(capitulo.nome)
        for item in capitulo.itens:
            local = f"{ROTEIRO}:{item.linha}"
            if item.id in listados:
                problemas.append(f"{local}: '{item.id}' já está no roteiro, no capítulo '{listados[item.id]}'")
                continue
            listados[item.id] = capitulo.nome
            trecho = trechos.get(item.id)
            if trecho is None:
                problemas.append(f"{local}: '{item.id}' não está marcado em nenhum documento")
                continue
            if trecho.arquivo != item.arquivo:
                problemas.append(f"{local}: '{item.id}' está em {trecho.arquivo}, não em {item.arquivo}")
            if trecho.capitulo != capitulo.nome:
                problemas.append(
                    f"{trecho.arquivo}:{trecho.linha}: '{item.id}' declara o capítulo '{trecho.capitulo}', "
                    f"mas o roteiro o põe em '{capitulo.nome}'"
                )
    for trecho in trechos.values():
        if trecho.id not in listados:
            problemas.append(f"{trecho.arquivo}:{trecho.linha}: o trecho '{trecho.id}' não está no roteiro ({ROTEIRO})")


def _reescrever_link(destino: str, origem: Path, saida: Path) -> str:
    if destino.startswith(("#", "/")) or ESQUEMA.match(destino):
        return destino
    caminho, separador, ancora = destino.partition("#")
    if not caminho:
        return destino
    novo = os.path.relpath(os.path.normpath(origem / unquote(caminho)), saida).replace(os.sep, "/")
    # Espaços (e parênteses) precisam ser codificados nos destinos Markdown.
    # A URL continua relativa e independe do caminho absoluto do checkout.
    novo = quote(novo, safe="/._-~")
    if caminho.endswith("/"):
        novo += "/"
    return novo + separador + ancora


def _reescrever_links(linha: str, origem: Path, saida: Path) -> str:
    """Reescreve os destinos relativos; num link [`caminho`](caminho), também o texto."""

    partes: list[str] = []
    fim = 0
    for link in LINK.finditer(linha):
        antes = linha[fim : link.start()]
        destino = link.group(2)
        novo = _reescrever_link(destino, origem, saida)
        rotulo = f"[`{destino}`"
        if novo != destino and antes.endswith(rotulo):
            antes = antes[: -len(rotulo)] + f"[`{novo}`"
        partes += [antes, link.group(1), novo, link.group(3)]
        fim = link.end()
    partes.append(linha[fim:])
    return "".join(partes)


def ajustar(trecho: Trecho, docs: Path, saida: Path) -> list[str]:
    """Títulos a partir do nível 2, sem a numeração da origem, e links relativos à saída.

    Com ``titulo`` no marcador, o primeiro título do trecho é trocado por ele (ou,
    se o trecho não tiver título, ganha um de nível 2).
    """

    linhas = list(trecho.linhas)
    while linhas and not linhas[0].strip():
        linhas.pop(0)
    while linhas and not linhas[-1].strip():
        linhas.pop()
    niveis: list[int] = []
    em_codigo = False
    for linha in linhas:
        if CERCA.match(linha):
            em_codigo = not em_codigo
        elif not em_codigo:
            titulo = TITULO.match(linha)
            if titulo:
                niveis.append(len(titulo.group(1)))
    deslocamento = 2 - min(niveis) if niveis else 0
    origem = (docs / trecho.arquivo).parent
    resultado: list[str] = []
    em_codigo = False
    titulo_pendente = trecho.titulo
    for linha in linhas:
        if CERCA.match(linha):
            em_codigo = not em_codigo
        elif not em_codigo:
            titulo = TITULO.match(linha)
            if titulo:
                nivel = max(1, min(6, len(titulo.group(1)) + deslocamento))
                texto = NUMERACAO.sub("", titulo.group(2))
                if titulo_pendente is not None:
                    texto, titulo_pendente = titulo_pendente, None
                linha = "#" * nivel + " " + texto
            linha = _reescrever_links(linha, origem, saida)
        resultado.append(linha)
    if titulo_pendente is not None and not niveis:
        resultado = [f"## {titulo_pendente}", "", *resultado]
    return resultado


def montar(capitulos: list[Capitulo], trechos: dict[str, Trecho], docs: Path, saida: Path) -> str:
    partes = [
        (
            "<!-- Rascunho gerado por Codigo/scripts/extrair_livro.py, na ordem de Documentacao/livro.md. "
            "Não edite este arquivo: edite os trechos marcados nos documentos de origem. -->"
        ),
    ]
    for capitulo in capitulos:
        partes += ["", f"# {capitulo.nome}"]
        for item in capitulo.itens:
            trecho = trechos[item.id]
            partes += ["", f"<!-- trecho: {trecho.id} | fonte: Documentacao/{trecho.arquivo}:{trecho.linha} -->", ""]
            partes += ajustar(trecho, docs, saida)
    return "\n".join(partes) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Confere a marcação do livro e monta o rascunho a partir da documentação.")
    parser.add_argument("--verificar", action="store_true", help="só confere a marcação e o roteiro")
    parser.add_argument("-o", "--saida", help="arquivo do rascunho (padrão: build/livro/livro.md; '-' para a saída padrão)")
    parser.add_argument("--capitulo", help="monta só este capítulo")
    parser.add_argument("--docs", type=Path, default=REPO_ROOT / "Documentacao", help="pasta da documentação")
    args = parser.parse_args(argv)

    docs = args.docs.resolve()
    problemas: list[str] = []
    trechos = ler_trechos(docs, problemas)
    capitulos = ler_roteiro(docs, problemas)
    conferir(capitulos, trechos, problemas)
    if args.capitulo is not None:
        capitulos = [capitulo for capitulo in capitulos if capitulo.nome == args.capitulo]
        if not capitulos:
            problemas.append(f"capítulo '{args.capitulo}' não está no roteiro")
    if problemas:
        for problema in problemas:
            print(f"livro: {problema}", file=sys.stderr)
        return 1

    total = sum(len(capitulo.itens) for capitulo in capitulos)
    if args.verificar:
        print(f"livro: marcação conferida, {len(capitulos)} capítulo(s) e {total} trecho(s)")
        return 0
    if args.saida == "-":
        sys.stdout.write(montar(capitulos, trechos, docs, Path.cwd()))
        return 0
    destino = Path(args.saida) if args.saida else REPO_ROOT / "build" / "livro" / "livro.md"
    destino = destino.resolve()
    destino.parent.mkdir(parents=True, exist_ok=True)
    destino.write_text(montar(capitulos, trechos, docs, destino.parent), encoding="utf-8")
    print(f"livro: {total} trecho(s) em {destino}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
