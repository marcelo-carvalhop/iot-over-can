"""Análise de uma campanha: dos registros brutos às tabelas, figuras e comparações.

Tudo o que aparece no relatório sai daqui, a partir de ``execucoes/*/traco.jsonl``:
apagar ``relatorio/`` e rodar de novo reproduz os mesmos números.

Para cada condição:
  - distribuições (ecos, intervalos): percentis das amostras de todas as
    repetições juntas e, como unidade de replicação, a média e o p99 de cada
    repetição, com IC 95 % entre repetições;
  - proporções (perdas): contagens somadas e intervalo de Wilson;
  - medidas escalares (detecção, recuperação, carga obtida): uma por repetição.
Comparações: cada condição contra a condição de referência que só difere no
fator comparado (por exemplo, iot=on contra iot=off com a mesma carga), com
diferença de médias, g de Hedges, delta de Cliff e Mann-Whitney.
"""
from __future__ import annotations

import csv
import hashlib
import json
import math
import os
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from typing import Any

from iocpesquisa import estatistica as st
from iocpesquisa.coleta import ler_traco, parametros_sem_semente
from iocpesquisa.metricas import calcular, resumo_bench

DISTRIBUICOES = ("eco_local_us", "eco_sensor_us", "eco_wifi_us", "eco_can_us", "hb_intervalo_us",
                 "tel_intervalo_us", "cripto_verificacao_us", "cripto_selo_us", "cripto_handshake_us",
                 "ponte_media_us", "ponte_max_us")
ESCALARES = ("carga_obtida_pct", "barramento_ocupacao_media_pct", "barramento_ocupacao_max_pct", "hb_jitter_us",
             "hb_desvio_max_us", "deteccao_sessao_ms", "deteccao_stale_ms", "deteccao_lost_ms",
             "decisao_reassociacao_ms", "restabelecimento_sessao_ms", "interrupcao_telemetria_ms",
             "retomada_telemetria_ms", "recuperacao_telemetria_ms", "tel_maior_intervalo_ms", "transicoes_espurias")
# Valor por repetição usado nas comparações entre condições.
COMPARAVEIS = {
    "eco_local_media_us": lambda m: st.mean(m["amostras"].get("eco_local_us") or []),
    "eco_local_p99_us": lambda m: st.percentile(m["amostras"].get("eco_local_us") or [], 99),
    "eco_sensor_media_us": lambda m: st.mean(m["amostras"].get("eco_sensor_us") or []),
    "eco_can_media_us": lambda m: st.mean(m["amostras"].get("eco_can_us") or []),
    "hb_jitter_us": lambda m: m.get("hb_jitter_us", math.nan),
    "tel_intervalo_p99_us": lambda m: st.percentile(m["amostras"].get("tel_intervalo_us") or [], 99),
    "tel_perda": lambda m: (m["tel_perdidas"] / (m["tel_recebidas"] + m["tel_perdidas"])
                            if m.get("tel_recebidas", 0) + m.get("tel_perdidas", 0) else math.nan),
    "ponte_media_us": lambda m: st.mean(m["amostras"].get("ponte_media_us") or []),
    "deteccao_sessao_ms": lambda m: m.get("deteccao_sessao_ms") if m.get("deteccao_sessao_ms") is not None else math.nan,
    "retomada_telemetria_ms": lambda m: (m.get("retomada_telemetria_ms")
                                         if m.get("retomada_telemetria_ms") is not None else math.nan),
}


def _ok(v: Any) -> bool:
    return v is not None and not (isinstance(v, float) and math.isnan(v))


def _fmt(v: Any, nd: int = 1) -> str:
    if not _ok(v):
        return "—"
    if isinstance(v, str):
        return v
    if isinstance(v, bool):
        return "sim" if v else "não"
    if isinstance(v, (int,)) and not isinstance(v, bool):
        return str(v)
    return f"{v:.{nd}f}"


def _versao_metricas() -> str:
    """Hash do código que define as métricas: um cache calculado por outra versão é refeito."""
    h = hashlib.sha256()
    for nome in ("metricas.py", "traco.py", "carga.py"):
        h.update((Path(__file__).parent / nome).read_bytes())
    return h.hexdigest()[:16]


VERSAO_METRICAS = _versao_metricas()


def _metricas_de(pasta: str) -> dict[str, Any]:
    cab, regs = ler_traco(Path(pasta) / "traco.jsonl")
    met = calcular(cab, regs)
    met["versao_metricas"] = VERSAO_METRICAS
    (Path(pasta) / "metricas.json").write_text(json.dumps(met, ensure_ascii=False) + "\n", encoding="utf-8")
    return met


def _cache_valido(pasta: Path) -> dict[str, Any] | None:
    cache, traco = pasta / "metricas.json", pasta / "traco.jsonl"
    if not cache.exists() or cache.stat().st_mtime < traco.stat().st_mtime:
        return None
    met = json.loads(cache.read_text(encoding="utf-8"))
    return met if met.get("versao_metricas") == VERSAO_METRICAS else None


def carregar_execucoes(base: Path, paralelo: int | None = None,
                       ) -> tuple[list[tuple[dict[str, Any], dict[str, Any]]], list[dict[str, Any]]]:
    """(manifesto, métricas) de cada execução completa; e as que falharam.

    As métricas ficam em ``metricas.json`` ao lado do registro; as que faltam
    (ou vieram de outra versão do código das métricas) são calculadas em
    paralelo."""
    boas: list[tuple[str, dict[str, Any], dict[str, Any] | None]] = []
    falhas = []
    for manifesto_path in sorted(base.glob("execucoes/*/manifesto.json")):
        m = json.loads(manifesto_path.read_text(encoding="utf-8"))
        if m.get("status") != "ok":
            falhas.append(m)
            continue
        boas.append((str(manifesto_path.parent), m, _cache_valido(manifesto_path.parent)))
    faltam = [pasta for pasta, _, met in boas if met is None]
    calculadas: dict[str, dict[str, Any]] = {}
    trabalhadores = min(paralelo or os.cpu_count() or 1, len(faltam))
    if trabalhadores > 1:
        with ProcessPoolExecutor(max_workers=trabalhadores) as pool:
            calculadas = dict(zip(faltam, pool.map(_metricas_de, faltam, chunksize=8)))
    else:
        calculadas = {pasta: _metricas_de(pasta) for pasta in faltam}
    return [(m, met if met is not None else calculadas[pasta]) for pasta, m, met in boas], falhas


def agregar(execs: list[tuple[dict[str, Any], dict[str, Any]]]) -> list[dict[str, Any]]:
    grupos: dict[tuple[str, str], list[tuple[dict[str, Any], dict[str, Any]]]] = {}
    for m, met in execs:
        grupos.setdefault((m["bloco"], m["condicao"]), []).append((m, met))
    linhas = []
    for (bloco, cond), lst in sorted(grupos.items()):
        params = parametros_sem_semente(lst[0][0]["parametros"])
        linha: dict[str, Any] = {"bloco": bloco, "condicao": cond, "repeticoes": len(lst), **params,
                                 "hash_configuracao": lst[0][0].get("hash_configuracao", ""),
                                 "hash_configuracao_unico": len({m.get("hash_configuracao") for m, _ in lst}) == 1}
        for chave in DISTRIBUICOES:
            por_exec = [met["amostras"].get(chave) or [] for _, met in lst]
            juntas = [x for xs in por_exec for x in xs]
            if not juntas:
                continue
            resumo = st.summary(juntas)
            for k in ("n", "p50", "p95", "p99", "max"):
                linha[f"{chave}_{k}"] = resumo[k]
            medias = [st.mean(xs) for xs in por_exec if xs]
            p99s = [st.percentile(xs, 99) for xs in por_exec if xs]
            linha[f"{chave}_media"] = st.mean(medias)
            linha[f"{chave}_media_ic95_inf"], linha[f"{chave}_media_ic95_sup"] = st.ci_mean(medias)
            linha[f"{chave}_p99_rep_media"] = st.mean(p99s)
            linha[f"{chave}_p99_rep_ic95_inf"], linha[f"{chave}_p99_rep_ic95_sup"] = st.ci_mean(p99s)
        for chave in ("eco_local_us", "eco_sensor_us"):
            enviados = sum(met.get(f"{chave}_enviados", 0) for _, met in lst)
            perdidos = sum(met.get(f"{chave}_perdidos", 0) for _, met in lst)
            if enviados:
                p, lo, hi = st.wilson(perdidos, enviados)
                linha[f"{chave}_perda"], linha[f"{chave}_perda_ic95_inf"], linha[f"{chave}_perda_ic95_sup"] = p, lo, hi
        recebidas = sum(met.get("tel_recebidas", 0) for _, met in lst)
        perdidas = sum(met.get("tel_perdidas", 0) for _, met in lst)
        if recebidas + perdidas:
            p, lo, hi = st.wilson(perdidas, recebidas + perdidas)
            linha["tel_perda"], linha["tel_perda_ic95_inf"], linha["tel_perda_ic95_sup"] = p, lo, hi
            linha["tel_amostras"] = recebidas + perdidas
        for chave in ESCALARES:
            vals = [float(met[chave]) for _, met in lst if _ok(met.get(chave))]
            if not vals:
                continue
            resumo = st.summary(vals)
            linha[f"{chave}_n"] = resumo["n"]
            linha[f"{chave}_media"] = resumo["media"]
            linha[f"{chave}_ic95_inf"], linha[f"{chave}_ic95_sup"] = resumo["ic95_inf"], resumo["ic95_sup"]
            linha[f"{chave}_p50"], linha[f"{chave}_max"] = resumo["p50"], resumo["max"]
            nao_detectadas = sum(1 for _, met in lst if chave in met and met[chave] is None)
            if nao_detectadas:
                linha[f"{chave}_nao_observado"] = nao_detectadas
        confere = [met["carga_sequencia_confere"] for _, met in lst if "carga_sequencia_confere" in met]
        if confere:
            linha["carga_sequencia_confere"] = all(confere)
        linhas.append(linha)
    return linhas


def comparar(desenho: dict[str, Any], execs: list[tuple[dict[str, Any], dict[str, Any]]]) -> list[dict[str, Any]]:
    grupos: dict[tuple[str, str], list[tuple[dict[str, Any], dict[str, Any]]]] = {}
    for m, met in execs:
        grupos.setdefault((m["bloco"], m["condicao"]), []).append((m, met))
    params_de = {k: parametros_sem_semente(v[0][0]["parametros"]) for k, v in grupos.items()}
    saida = []
    for comp in desenho.get("comparacoes", []):
        fator, base = comp["fator"], comp["base"]
        ignorar = set(comp.get("ignorar", []))  # parâmetros sem sentido no nível de referência

        def chave(p: dict[str, Any], ignorar: set[str] = ignorar) -> dict[str, str]:
            return {kk: str(vv) for kk, vv in p.items() if kk not in ignorar}

        for (bloco, cond), params in sorted(params_de.items()):
            if fator not in params or str(params[fator]) == str(base):
                continue
            alvo = chave({**params, fator: base})
            ref = next((k for k, p in params_de.items() if k[0] == bloco and chave(p) == alvo), None)
            if ref is None:
                continue
            for nome, fn in COMPARAVEIS.items():
                a = [v for v in (fn(met) for _, met in grupos[ref]) if _ok(v)]
                b = [v for v in (fn(met) for _, met in grupos[(bloco, cond)]) if _ok(v)]
                if len(a) < 2 or len(b) < 2:
                    continue
                g, g_lo, g_hi = st.hedges_g(a, b)
                delta, mag = st.cliffs_delta(a, b)
                _, p = st.mann_whitney(a, b)
                saida.append({
                    "bloco": bloco, "fator": fator, "base": base, "nivel": params[fator],
                    "condicao": cond, "referencia": ref[1], "metrica": nome,
                    "n_ref": len(a), "n": len(b), "media_ref": st.mean(a), "media": st.mean(b),
                    "diferenca": st.mean(b) - st.mean(a), "hedges_g": g, "hedges_g_ic95_inf": g_lo,
                    "hedges_g_ic95_sup": g_hi, "cliff_delta": delta, "cliff_magnitude": mag, "mann_whitney_p": p,
                })
    # Muitas comparações por métrica: valor-p ajustado (Holm) para afirmações de significância.
    for metrica in {c["metrica"] for c in saida}:
        grupo = [c for c in saida if c["metrica"] == metrica]
        for c, ajustado in zip(grupo, st.holm([c["mann_whitney_p"] for c in grupo]), strict=True):
            c["mann_whitney_p_holm"] = ajustado
    return saida


def _csv(caminho: Path, linhas: list[dict[str, Any]]) -> None:
    caminho.parent.mkdir(parents=True, exist_ok=True)
    chaves: list[str] = []
    for linha in linhas:
        for k in linha:
            if k not in chaves:
                chaves.append(k)
    with caminho.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=chaves)
        w.writeheader()
        for linha in linhas:
            w.writerow({k: ("" if not _ok(v) else v) for k, v in linha.items()})


def _figuras(dest: Path, execs: list[tuple[dict[str, Any], dict[str, Any]]]) -> list[str]:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return []
    feitas = []
    dest.mkdir(parents=True, exist_ok=True)
    grupos: dict[str, list[float]] = {}
    for m, met in execs:
        grupos.setdefault(m["condicao"], []).extend(met["amostras"].get("eco_local_us") or [])
    grupos = {k: v for k, v in grupos.items() if v}
    if grupos:
        fig, ax = plt.subplots(figsize=(8, 5))
        for cond, vals in sorted(grupos.items())[:12]:
            xs = sorted(v / 1000.0 for v in vals)
            ax.step(xs, [(i + 1) / len(xs) for i in range(len(xs))], where="post", label=cond, linewidth=1.2)
        ax.set_xlabel("ida e volta no CAN, eco LOCAL (ms)")
        ax.set_ylabel("fração acumulada")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=6, loc="lower right")
        fig.tight_layout()
        fig.savefig(dest / "eco_local_cdf.png", dpi=150)
        plt.close(fig)
        feitas.append("figuras/eco_local_cdf.png")
    jitter: dict[str, list[float]] = {}
    for m, met in execs:
        if _ok(met.get("hb_jitter_us")):
            jitter.setdefault(m["condicao"], []).append(met["hb_jitter_us"])
    if jitter:
        nomes = sorted(jitter)[:16]
        fig, ax = plt.subplots(figsize=(8, 5))
        ax.boxplot([jitter[n] for n in nomes], vert=False)
        ax.set_yticks(range(1, len(nomes) + 1), labels=nomes, fontsize=6)
        ax.set_xlabel("variação do intervalo do sinal de presença por repetição (desvio, µs)")
        ax.grid(alpha=0.3)
        fig.tight_layout()
        fig.savefig(dest / "hb_jitter.png", dpi=150)
        plt.close(fig)
        feitas.append("figuras/hb_jitter.png")
    return feitas


def _tabela(linhas: list[dict[str, Any]], colunas: list[tuple[str, str, int]]) -> str:
    cab = "| condição | " + " | ".join(t for t, _, _ in colunas) + " |"
    sep = "|---|" + "---|" * len(colunas)
    corpo = []
    for linha in linhas:
        if not any(_ok(linha.get(c)) for _, c, _ in colunas):
            continue
        corpo.append(f"| {linha['bloco']}: {linha['condicao']} | " +
                     " | ".join(_fmt(linha.get(c), nd) for _, c, nd in colunas) + " |")
    return "\n".join([cab, sep, *corpo]) if corpo else "_sem dados_"


def _ic(linha: dict[str, Any], chave: str, escala: float = 1.0, nd: int = 2) -> str:
    m, lo, hi = linha.get(f"{chave}"), linha.get(f"{chave}_ic95_inf"), linha.get(f"{chave}_ic95_sup")
    if not _ok(m):
        return "—"
    if not _ok(lo):
        return f"{m * escala:.{nd}f}"
    return f"{m * escala:.{nd}f} [{lo * escala:.{nd}f}; {hi * escala:.{nd}f}]"


def _eco_ms(c: dict[str, Any]) -> dict[str, Any]:
    def ms(chave: str) -> float | None:
        v = c.get(chave)
        return v / 1000.0 if _ok(v) else None
    return {**c, "eco_local_ic": _ic(c, "eco_local_us_media", 1e-3), "eco_sensor_ic": _ic(c, "eco_sensor_us_media", 1e-3),
            "eco_local_p50_ms": ms("eco_local_us_p50"), "eco_local_p99_ms": ms("eco_local_us_p99"),
            "eco_sensor_p99_ms": ms("eco_sensor_us_p99"), "eco_wifi_media_ms": ms("eco_wifi_us_media"),
            "eco_can_media_ms": ms("eco_can_us_media")}


def analisar(base: Path, figuras: bool = True) -> dict[str, Any]:
    desenho = json.loads((base / "desenho.json").read_text(encoding="utf-8"))
    plano = json.loads((base / "plano.json").read_text(encoding="utf-8"))
    amb = json.loads((base / "ambiente.json").read_text(encoding="utf-8"))
    execs, falhas = carregar_execucoes(base)
    condicoes = agregar(execs)
    comps = comparar(desenho, execs)
    dest = base / "relatorio"
    _csv(dest / "condicoes.csv", condicoes)
    _csv(dest / "comparacoes.csv", comps)
    bench = []
    for arq in sorted((base / "bench").glob("*.jsonl")) if (base / "bench").exists() else []:
        _, regs = ler_traco(arq)
        bench.extend(resumo_bench(regs))
    if bench:
        _csv(dest / "criptografia.csv", bench)
    imagens = _figuras(dest / "figuras", execs) if figuras else []

    planejadas = len(plano["execucoes"])
    config = next((m.get("config_firmware") for m, _ in execs if m.get("config_firmware")), {})
    seq_ok = [c.get("carga_sequencia_confere") for c in condicoes if "carga_sequencia_confere" in c]
    texto = [
        f"# Relatório da campanha `{desenho.get('nome')}`",
        "",
        desenho.get("descricao", ""),
        "",
        "## Proveniência",
        "",
        f"- Bancada: **{desenho.get('bancada', 'virtual')}**; desenho `sha256:{plano['desenho_sha256'][:16]}`",
        f"- Código: commit `{(amb['git'].get('commit') or '?')[:12]}` ({amb['git'].get('ramo')})"
        + (" **com alterações não gravadas**" if amb["git"].get("alteracoes_nao_gravadas") else ""),
        f"- Fontes do firmware: `sha256:{amb['fontes_firmware_sha256'][:16]}`; Python {amb['python']}",
        "- Parâmetros da compilação informados pela Probe ([GW] CONFIG): "
        + (", ".join(f"`{k}={v}`" for k, v in sorted(config.items())) if config else "—"),
        f"- Execuções: {len(execs)} completas de {planejadas} planejadas; {len(falhas)} com erro",
        "- Amostragem do eco: " + (", ".join(sorted({met["eco_amostragem"] for _, met in execs
                                                     if met.get("eco_amostragem")})) or "—")
        + "; fase do início da janela sorteada por execução (`phase_ms`)",
        (f"- Configuração idêntica entre as repetições de cada condição: "
        f"{'sim' if all(c['hash_configuracao_unico'] for c in condicoes) else '**não** (ver condicoes.csv)'}"),
        (f"- Sequência de carga conferida pela semente: "
        f"{'sim, em todas as condições com carga' if seq_ok and all(seq_ok) else ('—' if not seq_ok else '**não**')}"),
        "",
        "## Ida e volta no barramento (eco), relógio da Probe",
        "",
        "Média por repetição com IC 95 % entre repetições; percentis com todas as amostras juntas (ms).",
        "",
        _tabela([_eco_ms(c) for c in condicoes],
                [("LOCAL média [IC95]", "eco_local_ic", 0), ("LOCAL p50", "eco_local_p50_ms", 2),
                 ("LOCAL p99", "eco_local_p99_ms", 2), ("LOCAL perda", "eco_local_us_perda", 3),
                 ("SENSOR média [IC95]", "eco_sensor_ic", 0), ("SENSOR p99", "eco_sensor_p99_ms", 2),
                 ("parte Wi-Fi média", "eco_wifi_media_ms", 2), ("parte CAN média", "eco_can_media_ms", 2)]),
        "",
        "## Sinal de presença do líder e telemetria",
        "",
        _tabela(condicoes, [("intervalo HB p50 (µs)", "hb_intervalo_us_p50", 0),
                            ("HB p99 (µs)", "hb_intervalo_us_p99", 0),
                            ("variação HB média (µs)", "hb_jitter_us_media", 1),
                            ("telemetria perda", "tel_perda", 4), ("amostras", "tel_amostras", 0),
                            ("ocupação média (%)", "barramento_ocupacao_media_pct_media", 2),
                            ("carga obtida (%)", "carga_obtida_pct_media", 2)]),
        "",
        "## Máquina de estados e recuperação (relógio de referência, ms)",
        "",
        "Tempos contados a partir da falha (instante de referência), média entre repetições.",
        "",
        _tabela(condicoes, [("perda da sessão", "deteccao_sessao_ms_media", 0),
                            ("STALE", "deteccao_stale_ms_media", 0), ("LOST", "deteccao_lost_ms_media", 0),
                            ("decisão de reassociação", "decisao_reassociacao_ms_media", 0),
                            ("sessão restabelecida", "restabelecimento_sessao_ms_media", 0),
                            ("telemetria interrompida", "interrupcao_telemetria_ms_media", 0),
                            ("telemetria retomada", "retomada_telemetria_ms_media", 0),
                            ("após o fim da causa", "recuperacao_telemetria_ms_media", 0),
                            ("transições espúrias", "transicoes_espurias_media", 2)]),
        "",
        "## Custo da criptografia e da ponte no Node (µs)",
        "",
        *([("Na bancada virtual o tempo de CPU não é modelado: a criptografia não aparece aqui (os valores do "
           "CSV são artefato do relógio simulado). A ponte reflete só a espera na fila de transmissão do Node."),
           "",
           _tabela(condicoes, [("ponte Wi-Fi→CAN média", "ponte_media_us_media", 0),
                               ("ponte p99", "ponte_max_us_p99", 0)])]
          if desenho.get("bancada", "virtual") == "virtual" else
          [_tabela(condicoes, [("verificação HMAC média", "cripto_verificacao_us_media", 1),
                               ("selo HMAC média", "cripto_selo_us_media", 1),
                               ("handshake (CPU)", "cripto_handshake_us_media", 0),
                               ("ponte Wi-Fi→CAN média", "ponte_media_us_media", 0),
                               ("ponte p99", "ponte_max_us_p99", 0)])]),
    ]
    if bench:
        texto += ["", "### Medição isolada (BENCH CRYPTO)", "",
                  "| placa | operação | bytes | n | média | p50 | p95 | p99 | máx |", "|---|---|---|---|---|---|---|---|---|"]
        texto += [f"| {b['domain']} | {b['op']} | {b['bytes']} | {b['n']} | {_fmt(b['mean_us'], 2)} | "
                  f"{_fmt(b['p50_us'], 0)} | {_fmt(b['p95_us'], 0)} | {_fmt(b['p99_us'], 0)} | {_fmt(b['max_us'], 0)} |"
                  for b in bench]
    if comps:
        texto += ["", "## Comparações (unidade: repetição)", "",
                  "| condição | contra | métrica | média ref. | média | g de Hedges [IC95] | delta de Cliff | p (MW, Holm) |",
                  "|---|---|---|---|---|---|---|---|"]
        for c in comps:
            texto.append(f"| {c['condicao']} | {c['referencia']} | {c['metrica']} | {_fmt(c['media_ref'], 1)} | "
                         f"{_fmt(c['media'], 1)} | {_fmt(c['hedges_g'], 2)} [{_fmt(c['hedges_g_ic95_inf'], 2)}; "
                         f"{_fmt(c['hedges_g_ic95_sup'], 2)}] | {_fmt(c['cliff_delta'], 2)} ({c['cliff_magnitude']}) | "
                         f"{_fmt(c['mann_whitney_p_holm'], 4)} |")
    if imagens:
        texto += ["", "## Figuras", ""] + [f"![{i}]({i})" for i in imagens]
    texto += [
        "",
        "## Reprodutibilidade",
        "",
        ("O caminho da análise é automático: `python -m iocpesquisa analisar <pasta>` refaz este relatório, as "
        "tabelas CSV e as figuras a partir dos registros brutos (`execucoes/*/traco.jsonl`). Na bancada virtual, "
        "a coleta também é: a mesma semente produz o mesmo registro. Na bancada física, a coleta exige o "
        "equipamento descrito em Documentacao/pesquisa/metodologia.md; o plano, as sementes e a ordem são os mesmos."),
        "",
        "## Ressalvas",
        "",
        ("- Bancada virtual: executa o firmware real dos Nodes, mas não modela o tempo de CPU (a criptografia "
        "aparece com custo zero), o rádio nem a temporização do MCP2515 por SPI."),
        ("- Os carimbos da Probe têm a latência do laço principal (da ordem de 1 ms): abaixo disso, a medida "
        "não distingue efeitos de arbitragem (o pior caso analítico está em Front/pico_tui/metrics/rta.py)."),
        "- Intervalos de relógios diferentes (Probe e Node) são subtraídos (eco_can); instantes, nunca.",
    ]
    (dest / "relatorio.md").write_text("\n".join(texto) + "\n", encoding="utf-8")
    return {"condicoes": len(condicoes), "execucoes": len(execs), "falhas": len(falhas), "comparacoes": len(comps),
            "figuras": imagens, "relatorio": str(dest / "relatorio.md")}


def sorteios_repetidos(execs: list[tuple[dict[str, Any], dict[str, Any]]]) -> list[str]:
    """Condições em que as repetições não sortearam valores próprios (semente, semente do eco, fase).

    Repetições com os mesmos sorteios refazem a mesma execução: a variação entre
    elas some, o IC sai com largura zero e qualquer diferença vira "significativa"
    (pseudorreplicação). Na bancada virtual, que é determinística, só os sorteios
    distinguem uma repetição da outra."""
    grupos: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for m, _ in execs:
        grupos.setdefault((m["bloco"], m["condicao"]), []).append(m.get("parametros", {}))
    problemas = []
    for (bloco, cond), lista in sorted(grupos.items()):
        if len(lista) < 2:
            continue
        for chave in ("seed", "echo_seed", "phase_ms"):
            vals = [p.get(chave) for p in lista]
            if all(v is None for v in vals):
                problemas.append(f"{bloco}: {cond}: {chave} não sorteado por repetição (plano anterior a esse sorteio)")
            elif chave != "phase_ms" and len(set(map(str, vals))) < len(vals):
                # A fase (0 a 5 s) pode repetir por acaso; as sementes de 32 bits, não.
                problemas.append(f"{bloco}: {cond}: {chave} repetida entre repetições (pseudorreplicação)")
    return problemas


def verificar(base: Path) -> list[str]:
    """Problemas de reprodutibilidade ou de validade encontrados na campanha (lista vazia = nada a apontar)."""
    problemas = []
    desenho = json.loads((base / "desenho.json").read_text(encoding="utf-8"))
    from iocpesquisa.desenho import plano as gerar_plano
    if gerar_plano(desenho)["desenho_sha256"] != json.loads((base / "plano.json").read_text())["desenho_sha256"]:
        problemas.append("plano.json não corresponde ao desenho.json")
    execs, falhas = carregar_execucoes(base)
    problemas += [f"{m['execucao']}: {m.get('erro') or 'erro'}" for m in falhas]
    por_cond: dict[tuple[str, str], set[str]] = {}
    sujas = 0
    for m, met in execs:
        por_cond.setdefault((m["bloco"], m["condicao"]), set()).add(m.get("hash_configuracao", ""))
        if met.get("carga_sequencia_confere") is False:
            problemas.append(f"{m['execucao']}: a carga gerada não corresponde à semente declarada")
        if met.get("falha_sem_referencia"):
            problemas.append(f"{m['execucao']}: falha pedida sem instante de referência no registro")
        if m.get("git", {}).get("alteracoes_nao_gravadas"):
            sujas += 1
    for cond, hashes in por_cond.items():
        if len(hashes) > 1:
            problemas.append(f"{cond[0]}: {cond[1]}: repetições com configurações diferentes")
    problemas += sorteios_repetidos(execs)
    periodicas = sum(1 for _, met in execs if "PERIODIC" in (met.get("eco_amostragem") or ""))
    if periodicas:
        problemas.append(f"{periodicas} execução(ões) com eco de período fixo: as sondas podem entrar em fase com as "
                         "atividades periódicas da rede (use echo_sampling=poisson)")
    if sujas:
        problemas.append(f"{sujas} execução(ões) coletada(s) com alterações não gravadas no código "
                         "(grave um commit antes de uma campanha que vá para o texto)")
    return problemas
