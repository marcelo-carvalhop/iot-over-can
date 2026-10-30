"""Métricas de uma execução, com definição operacional e domínio de relógio.

Todas as medidas são feitas dentro da janela de medição. As definições
estão também em Documentacao/pesquisa/metodologia.md (tabela de métricas).

| métrica                 | definição                                                           | relógio |
|-------------------------|---------------------------------------------------------------------|---------|
| eco_local_us            | ECHO LOCAL: envio do comando 0x304 até a confirmação remontada      | Probe   |
| eco_sensor_us           | ECHO SENSOR: idem, passando pelo sensor (PING/PONG pela sessão)     | Probe   |
| eco_wifi_us             | parte Wi-Fi do eco SENSOR, medida pelo Node                         | Node    |
| eco_can_us              | eco_sensor_us − eco_wifi_us (CAN + processamento no Node)           | misto (intervalos) |
| hb_intervalo_us         | intervalo entre sinais de presença do líder, como a Probe os trata  | Probe   |
| tel_intervalo_us        | intervalo entre amostras de telemetria remontadas                   | Probe   |
| tel_perda               | amostras encaminhadas pelo Node e não remontadas (saltos de SEQ)    | contagem |
| cripto_*_us             | HMAC por datagrama e CPU do handshake, no Node                       | Node    |
| ponte_us                | datagrama lido pelo Node até o último segmento entregue ao CAN      | Node    |
| deteccao_*_ms, recuperacao_*_ms | da verdade de referência à reação da rede                   | referência |
"""
from __future__ import annotations

import math
from itertools import pairwise
from typing import Any

from iocpesquisa import carga, estatistica
from iocpesquisa.traco import Evento, eventos, janela

UUID_PADRAO = "0xE6616408432B6F39"


def _nao_negativo(e: Evento, campo: str) -> bool:
    v = e.num(campo)
    return v is not None and v >= 0


def _sensor_alvo(params: dict[str, Any]) -> tuple[int, int, str]:
    alvo = str(params.get("alvo", "1.1"))
    node, _, child = alvo.partition(".")
    return int(node), int(child or 1), str(params.get("alvo_uuid", UUID_PADRAO)).upper().replace("0X", "0x")


def calcular(cabecalho: dict[str, Any], registros: list[dict[str, Any]]) -> dict[str, Any]:
    params = cabecalho.get("parametros", {})
    evs = eventos(registros)
    t_ini, t_fim = janela(evs)
    dentro = [e for e in evs if t_ini <= e.t_ref_ms <= t_fim]
    node, child, uuid = _sensor_alvo(params)
    out: dict[str, Any] = {"janela_ms": t_fim - t_ini}
    amostras: dict[str, list[float]] = {}

    # Ecos. A amostragem (POISSON ou PERIODIC) vem da linha ECHO_START da Probe.
    amostragens = sorted({e.campos.get("sampling", "PERIODIC") for e in dentro if e.tipo == "ECHO_START"})
    out["eco_amostragem"] = ",".join(amostragens) if amostragens else None
    for caminho in ("LOCAL", "SENSOR"):
        ecos = [e for e in dentro if e.tipo == "ECHO" and e.campos.get("path") == caminho]
        ok = [e for e in ecos if _nao_negativo(e, "rtt_us")]
        chave = "eco_local_us" if caminho == "LOCAL" else "eco_sensor_us"
        amostras[chave] = [e.num("rtt_us") for e in ok]
        out[f"{chave}_enviados"] = len(ecos)
        out[f"{chave}_perdidos"] = len(ecos) - len(ok)
        if caminho == "SENSOR":
            wl = [(e.num("rtt_us"), e.num("wl_us")) for e in ok if _nao_negativo(e, "wl_us")]
            amostras["eco_wifi_us"] = [w for _, w in wl]
            amostras["eco_can_us"] = [r - w for r, w in wl]

    # Sinal de presença do líder (relógio da Probe).
    hb = [e for e in dentro if e.tipo == "HEARTBEAT" and e.t_us is not None]
    intervalos = [b.t_us - a.t_us for a, b in pairwise(hb) if b.t_us > a.t_us]
    amostras["hb_intervalo_us"] = [float(x) for x in intervalos]
    periodo_ms = next((e.num("periodo") for e in hb if e.num("periodo")), None)
    if periodo_ms and intervalos:
        desvios = [abs(x - periodo_ms * 1000.0) for x in intervalos]
        out["hb_desvio_max_us"] = max(desvios)
        out["hb_jitter_us"] = estatistica.stdev(amostras["hb_intervalo_us"])

    # Telemetria do sensor alvo (por Node/filho em que estiver).
    tel = [e for e in dentro if e.tipo == "TEL"]
    por_origem: dict[tuple[int, int], list[Evento]] = {}
    for e in tel:
        por_origem.setdefault((int(e.num("node", 0)), int(e.num("child", 0))), []).append(e)
    perdidas = recebidas = 0
    tel_int: list[float] = []
    for seq in por_origem.values():
        recebidas += len(seq)
        for a, b in pairwise(seq):
            salto = (int(b.num("seq", 0)) - int(a.num("seq", 0))) % 65536
            if 1 < salto < 1000:
                perdidas += salto - 1
            if a.t_us is not None and b.t_us is not None and b.t_us > a.t_us:
                tel_int.append(float(b.t_us - a.t_us))
    amostras["tel_intervalo_us"] = tel_int
    out["tel_recebidas"] = recebidas
    out["tel_perdidas"] = perdidas

    # Carga gerada e sua sequência.
    fim_carga = next((e for e in evs if e.tipo == "FAULT_EVENT" and e.campos.get("kind") == "BUS_LOAD"
                      and e.campos.get("state") == "ENDED"), None)
    if fim_carga:
        quadros = int(fim_carga.num("frames", 0))
        semente = int(fim_carga.num("seed", 0))
        prio = fim_carga.campos.get("priority", "HIGH")
        hash_dev = int(fim_carga.campos.get("seq_hash", "0x0"), 16)
        out["carga_obtida_pct"] = fim_carga.num("achieved_pct")
        out["carga_quadros"] = quadros
        out["carga_semente"] = semente
        out["carga_sequencia_confere"] = carga.sequence_hash(semente, prio, quadros) == hash_dev
    ocup = [e.num("load") for e in dentro if e.tipo == "BUS_STATS" and e.num("load") is not None]
    if ocup:
        out["barramento_ocupacao_media_pct"] = estatistica.mean(ocup)
        out["barramento_ocupacao_max_pct"] = max(ocup)

    # Medidas do Node (criptografia e ponte).
    for campo, chave in (("crypto_rx_us", "cripto_verificacao_us"), ("crypto_tx_us", "cripto_selo_us"),
                         ("handshake_us", "cripto_handshake_us"), ("bridge_avg_us", "ponte_media_us"),
                         ("bridge_max_us", "ponte_max_us")):
        valores = [e.num(campo) for e in dentro if e.tipo == "STATS" and e.num("node") == node
                   and _nao_negativo(e, campo)]
        amostras[chave] = valores

    # Máquina de estados e reação a falhas.
    out.update(_falhas(evs, t_ini, t_fim, node, child, uuid, params))
    out["amostras"] = amostras
    return out


def _falhas(evs: list[Evento], t_ini: float, t_fim: float, node: int, child: int, uuid: str,
            params: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    uuid_up = uuid.upper()
    assoc = [e for e in evs if e.tipo == "ASSOC" and e.campos.get("uuid", "").upper() == uuid_up]
    links = [e for e in evs if e.tipo == "LINK" and int(e.num("node", 0)) == node and int(e.num("child", 0)) == child]
    em_assoc = {id(e) for e in assoc}
    em_links = {id(e) for e in links}
    # Onde o sensor alvo esteve (Node, filho): a telemetria dele depois de uma reassociação.
    origens = {(int(e.num("node", 0)), int(e.num("child", 0))) for e in assoc} | {(node, child)}

    def do_alvo(e: Evento) -> bool:
        return e.tipo == "TEL" and (int(e.num("node", 0)), int(e.num("child", 0))) in origens

    # Transições de estado do vínculo dentro da janela (para a contagem de espúrias).
    transicoes = 0
    anterior: str | None = None
    for e in assoc:
        estado = e.campos.get("state")
        if anterior is not None and estado != anterior and t_ini <= e.t_ref_ms <= t_fim and estado in ("STALE", "LOST"):
            transicoes += 1
        anterior = estado
    quedas_sessao = 0
    sec_ant: str | None = None
    for e in links:
        estado = e.campos.get("state")
        if sec_ant == "SECURE" and estado != "SECURE" and t_ini <= e.t_ref_ms <= t_fim:
            quedas_sessao += 1
        sec_ant = estado

    falha = params.get("fault", "none")
    if falha == "none":
        out["transicoes_espurias"] = transicoes + quedas_sessao
        return out

    # Verdade de referência: marca da bancada/operador ou o início do ensaio na Probe.
    t0 = t1 = None
    for e in evs:
        if e.tipo == "MARCA" and e.campos.get("verdade") == "SENSOR_OFF" and t0 is None:
            t0 = e.t_ref_ms
        if e.tipo == "MARCA" and e.campos.get("verdade") == "SENSOR_ON" and t1 is None:
            t1 = e.t_ref_ms
        if (e.tipo == "FAULT_EVENT" and e.campos.get("state") == "STARTED" and e.campos.get("kind") != "BUS_LOAD"
                and t0 is None):
            t0 = e.t_ref_ms
            dur = e.num("duration_ms", 0) or 0
            t1 = t0 + dur
    if t0 is None:
        out["falha_sem_referencia"] = True
        return out
    t1 = t1 if t1 is not None else t0

    def primeiro(pred, desde: float) -> float | None:
        for e in evs:
            if e.t_ref_ms >= desde and pred(e):
                return e.t_ref_ms - desde
        return None

    t_det = primeiro(
        lambda e: (id(e) in em_assoc and e.campos.get("session") == "NONE")
        or (id(e) in em_links and e.campos.get("state") != "SECURE"), t0)
    out["deteccao_sessao_ms"] = t_det
    out["deteccao_stale_ms"] = primeiro(lambda e: id(e) in em_assoc and e.campos.get("state") == "STALE", t0)
    out["deteccao_lost_ms"] = primeiro(lambda e: id(e) in em_assoc and e.campos.get("state") == "LOST", t0)
    out["decisao_reassociacao_ms"] = primeiro(
        lambda e: e.tipo == "FAILOVER" and e.campos.get("uuid", "").upper() == uuid_up, t0)
    # Sessão de volta (em qualquer Node), depois de a perda ter sido percebida; contado desde a falha.
    if t_det is not None:
        volta = primeiro(lambda e: id(e) in em_assoc and e.campos.get("session") == "SECURE", t0 + t_det)
        out["restabelecimento_sessao_ms"] = None if volta is None else t_det + volta
    else:
        out["restabelecimento_sessao_ms"] = None
    # Telemetria do sensor: a maior interrupção que começa perto da falha e quando ela termina.
    tel = [e.t_ref_ms for e in evs if do_alvo(e) and t0 - 2500 <= e.t_ref_ms <= t_fim]
    lacunas = [(b - a, b) for a, b in pairwise(tel) if b > t0]
    if lacunas:
        maior, fim = max(lacunas)
        out["interrupcao_telemetria_ms"] = maior
        out["retomada_telemetria_ms"] = fim - t0
    else:
        out["interrupcao_telemetria_ms"] = out["retomada_telemetria_ms"] = None
    # Depois do fim da causa (sensor religado, ponto de acesso de volta): só quando a causa termina.
    out["recuperacao_telemetria_ms"] = primeiro(do_alvo, t1) if t1 > t0 and falha in ("sensor_off", "ap_outage") else None
    todas = [e.t_ref_ms for e in evs if do_alvo(e) and t_ini <= e.t_ref_ms <= t_fim]
    gaps = [b - a for a, b in pairwise(todas)]
    out["tel_maior_intervalo_ms"] = max(gaps) if gaps else math.nan
    return out


def resumo_bench(registros: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Linhas BENCH (custo da criptografia em cada placa) de um registro."""
    out = []
    for e in eventos(registros):
        if e.tipo != "BENCH" or "op" not in e.campos:
            continue
        linha = {k: e.campos[k] for k in ("domain", "op", "bytes", "n")}
        for k in ("mean_us", "min_us", "p50_us", "p95_us", "p99_us", "max_us"):
            linha[k] = e.num(k)
        out.append(linha)
    return out
