"""Desenho experimental: blocos fatoriais, repetições, sementes e ordem.

Um arquivo de desenho (Pesquisa/desenhos/*.json) declara blocos; cada bloco
tem fatores (o produto cartesiano dá as condições), parâmetros fixos,
exclusões e o número de repetições. Cada execução recebe uma semente
derivada do desenho, da condição e da repetição, e a ordem das execuções é
sorteada com a semente base: o mesmo arquivo gera sempre o mesmo plano, e a
ordem aleatória evita confundir o efeito de um fator com a deriva do
equipamento ao longo da campanha.
"""
from __future__ import annotations

import hashlib
import itertools
import json
import random
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

PARAMETROS_CONHECIDOS = {
    "load_pct", "load_prio", "load_seed", "sensors", "iot", "warmup_s", "duration_s", "echo_n",
    "echo_period_ms", "echo_sampling", "echo_seed", "phase_ms", "fault", "fault_at_s", "fault_s", "alvo",
    "alvo_uuid", "seed",
} | set()
# Sorteados por execução (variam entre as repetições de uma mesma condição; não entram no hash de configuração).
SORTEADOS_POR_EXECUCAO = ("seed", "load_seed", "echo_seed", "phase_ms")
AMOSTRAGENS_ECO = {"poisson", "periodic"}
# Maior período das atividades periódicas da rede (relatório do enlace a 5 s): a fase sorteada do início da
# janela cobre esse intervalo inteiro.
FASE_MAX_MS = 5000
# Parâmetros de compilação (análise de sensibilidade): nome no desenho -> (define, campo da linha [GW] CONFIG).
PARAMETROS_FIRMWARE = {
    "fw_assoc_stale_ms": ("IOT_ASSOC_STALE_MS", "assoc_stale_ms"),
    "fw_assoc_lost_ms": ("IOT_ASSOC_LOST_MS", "assoc_lost_ms"),
    "fw_assoc_hysteresis_ms": ("IOT_ASSOC_HYSTERESIS_MS", "assoc_hysteresis_ms"),
    "fw_stream_period_ms": ("IOT_STREAM_PERIOD_MS", "stream_period_ms"),
}
PARAMETROS_CONHECIDOS |= set(PARAMETROS_FIRMWARE)


def firmware_de(params: dict[str, Any]) -> dict[str, int]:
    """Defines de compilação pedidos por uma execução (vazio = padrão das fontes)."""
    return {PARAMETROS_FIRMWARE[k][0]: int(v) for k, v in sorted(params.items()) if k in PARAMETROS_FIRMWARE}
FALHAS = {"none", "sensor_off", "owner_power", "ap_outage", "drop_session"}


@dataclass(frozen=True)
class Execucao:
    id: str
    bloco: str
    condicao: str
    repeticao: int
    semente: int
    ordem: int
    parametros: dict[str, Any] = field(default_factory=dict)

    def para_json(self) -> dict[str, Any]:
        return asdict(self)


def canonico(obj: Any) -> str:
    return json.dumps(obj, sort_keys=True, ensure_ascii=False, separators=(",", ":"))


def sha256_json(obj: Any) -> str:
    return hashlib.sha256(canonico(obj).encode()).hexdigest()


def condicao_id(fatores: dict[str, Any]) -> str:
    return ",".join(f"{k}={fatores[k]}" for k in sorted(fatores))


def semente(base: int, bloco: str, condicao: str, repeticao: int) -> int:
    digest = hashlib.sha256(f"{base}:{bloco}:{condicao}:{repeticao}".encode()).digest()
    value = int.from_bytes(digest[:4], "big")
    return value or 1


def derivada(semente_execucao: int, rotulo: str) -> int:
    """Semente própria de um sorteio da execução (eco, fase), independente da semente da carga."""
    digest = hashlib.sha256(f"{semente_execucao}:{rotulo}".encode()).digest()
    return int.from_bytes(digest[:4], "big") or 1


def sorteios(semente_execucao: int) -> dict[str, int]:
    """Sorteios por execução que quebram o alinhamento de fase entre a medida e as atividades periódicas."""
    return {"echo_seed": derivada(semente_execucao, "eco"),
            "phase_ms": derivada(semente_execucao, "fase") % FASE_MAX_MS}


def _excluida(valores: dict[str, Any], regras: list[dict[str, Any]]) -> bool:
    for regra in regras:
        if all(valores.get(k) in (v if isinstance(v, list) else [v]) for k, v in regra.items()):
            return True
    return False


def validar(desenho: dict[str, Any]) -> None:
    if not isinstance(desenho.get("blocos"), list) or not desenho["blocos"]:
        raise ValueError("o desenho precisa de pelo menos um bloco")
    if desenho.get("bancada", "virtual") not in ("virtual", "fisica"):
        raise ValueError("bancada deve ser 'virtual' ou 'fisica'")
    nomes = set()
    for bloco in desenho["blocos"]:
        nome = bloco.get("nome")
        if not nome or not isinstance(nome, str) or nome in nomes:
            raise ValueError("cada bloco precisa de um nome único")
        nomes.add(nome)
        reps = bloco.get("repeticoes", 0)
        if not isinstance(reps, int) or reps < 1:
            raise ValueError(f"bloco {nome}: repeticoes deve ser inteiro positivo")
        chaves = set(bloco.get("fatores", {})) | set(bloco.get("fixos", {}))
        desconhecidas = chaves - PARAMETROS_CONHECIDOS
        if desconhecidas:
            raise ValueError(f"bloco {nome}: parâmetros desconhecidos {sorted(desconhecidas)}")
        for fator, niveis in bloco.get("fatores", {}).items():
            if not isinstance(niveis, list) or not niveis:
                raise ValueError(f"bloco {nome}: o fator {fator} precisa de uma lista de níveis")
        falhas = set(bloco.get("fatores", {}).get("fault", [])) | {bloco.get("fixos", {}).get("fault", "none")}
        if falhas - FALHAS:
            raise ValueError(f"bloco {nome}: falhas desconhecidas {sorted(falhas - FALHAS)}")
        amostragens = set(bloco.get("fatores", {}).get("echo_sampling", [])) | {
            bloco.get("fixos", {}).get("echo_sampling", "poisson")}
        if amostragens - AMOSTRAGENS_ECO:
            raise ValueError(f"bloco {nome}: echo_sampling deve ser 'poisson' ou 'periodic'")
    for comp in desenho.get("comparacoes", []):
        if "fator" not in comp or "base" not in comp:
            raise ValueError("cada comparação precisa de 'fator' e 'base'")


def carregar(caminho: str | Path) -> dict[str, Any]:
    desenho = json.loads(Path(caminho).read_text(encoding="utf-8"))
    validar(desenho)
    return desenho


def expandir(desenho: dict[str, Any]) -> list[Execucao]:
    """Plano completo: todas as execuções, com semente e ordem."""
    validar(desenho)
    base = int(desenho.get("semente_base", 1))
    execucoes: list[Execucao] = []
    for bloco in desenho["blocos"]:
        fatores = bloco.get("fatores", {})
        fixos = bloco.get("fixos", {})
        nomes = sorted(fatores)
        for combo in itertools.product(*(fatores[n] for n in nomes)):
            valores = dict(zip(nomes, combo))
            if _excluida({**fixos, **valores}, bloco.get("excluir", [])):
                continue
            cid = condicao_id(valores) or "unica"
            for rep in range(1, bloco["repeticoes"] + 1):
                s = semente(base, bloco["nome"], cid, rep)
                params = {**fixos, **valores, "seed": s}
                params.setdefault("load_seed", s)
                for chave, valor in sorteios(s).items():
                    params.setdefault(chave, valor)
                execucoes.append(Execucao(
                    id=f"{bloco['nome']}-{hashlib.sha256(cid.encode()).hexdigest()[:6]}-r{rep:03d}",
                    bloco=bloco["nome"], condicao=cid, repeticao=rep, semente=s, ordem=0, parametros=params))
    ordem = list(range(len(execucoes)))
    if desenho.get("ordem_aleatoria", True):
        random.Random(base).shuffle(ordem)
    return [Execucao(**{**e.para_json(), "ordem": ordem[i]}) for i, e in enumerate(execucoes)]


def plano(desenho: dict[str, Any]) -> dict[str, Any]:
    execucoes = sorted(expandir(desenho), key=lambda e: e.ordem)
    condicoes = sorted({(e.bloco, e.condicao) for e in execucoes})
    return {
        "desenho": desenho.get("nome", "sem_nome"),
        "desenho_sha256": sha256_json(desenho),
        "condicoes": len(condicoes),
        "execucoes": [e.para_json() for e in execucoes],
    }
