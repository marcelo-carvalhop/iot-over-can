"""Coleta: executa o plano e grava o registro bruto de cada execução.

Registro bruto (``traco.jsonl``), um objeto por linha:

  {"tipo": "cabecalho", "execucao": ..., "parametros": {...}, "bancada": "virtual"|"fisica", ...}
  {"tipo": "linha", "fonte": "probe"|"bancada"|"coletor", "t_ref_ms": 1234.5, "t_host_ns": ..., "linha": "..."}

``fonte`` diz quem escreveu a linha: a Probe 00 (o que o firmware imprimiu,
com o carimbo ``t_us`` do relógio dela dentro da linha), a bancada virtual
(verdade de referência dos ensaios, como o instante em que o sensor foi
desligado) ou o próprio coletor (comandos enviados, marcas da janela,
confirmações do operador). ``t_ref_ms`` é o relógio de referência da execução:
o relógio da simulação na bancada virtual; na física, o relógio monotônico do
computador desde o início da execução (com o atraso da serial, da ordem de
milissegundos). ``t_host_ns`` só existe na bancada física.

Cada execução tem também ``manifesto.json`` com parâmetros, semente,
proveniência e o hash de configuração. Uma campanha interrompida continua de
onde parou: execuções com manifesto completo não são refeitas.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import threading
import time
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Protocol

from iocpesquisa.desenho import (
    PARAMETROS_FIRMWARE,
    SORTEADOS_POR_EXECUCAO,
    Execucao,
    expandir,
    firmware_de,
    plano,
    sha256_json,
    sorteios,
)
from iocpesquisa.proveniencia import (
    ambiente,
    hash_configuracao,
    parametros_config,
    raiz_repositorio,
)

STAMP = re.compile(r"^@(\d+) (.*)$")
MARK = re.compile(r"^# t=(\d+) pesquisa (.*)$")
ALVO_PADRAO = "1.1"
UUID_PADRAO = "0xE6616408432B6F39"  # sensor A da bancada virtual


def _agora() -> str:
    return datetime.now(UTC).isoformat(timespec="seconds")


def parametros_sem_semente(params: dict[str, Any]) -> dict[str, Any]:
    return {k: v for k, v in params.items() if k not in SORTEADOS_POR_EXECUCAO}


def com_sorteios(params: dict[str, Any]) -> dict[str, Any]:
    """Parâmetros da execução com os sorteios que faltarem (planos anteriores a eles, testes)."""
    return {**sorteios(int(params.get("seed", 1))), **params}


def comandos_eco(params: dict[str, Any]) -> tuple[str, str] | None:
    """Comandos dos dois trens de eco (LOCAL e SENSOR), iguais nas duas bancadas.

    Amostragem de Poisson por padrão (ver Codigo/node-can/include/pesquisa.h);
    cada trem tem a própria semente para que as sequências de intervalos não
    se repitam entre eles."""
    eco_n = int(params.get("echo_n", 0))
    if not eco_n:
        return None
    alvo = str(params.get("alvo", ALVO_PADRAO))
    periodo = int(params.get("echo_period_ms", 500))
    if params.get("echo_sampling", "poisson") == "periodic":
        return (f"ECHO {alvo} {eco_n} {periodo} LOCAL", f"ECHO {alvo} {eco_n} {periodo} SENSOR")
    s = int(com_sorteios(params)["echo_seed"]) & 0xFFFFFFFF or 1
    s_sensor = (s + 1) & 0xFFFFFFFF or 1
    return (f"ECHO {alvo} {eco_n} {periodo} LOCAL {s}", f"ECHO {alvo} {eco_n} {periodo} SENSOR {s_sensor}")


@dataclass
class Registro:
    caminho: Path
    linhas: int = 0
    _arquivo: Any = field(default=None, repr=False)

    def __enter__(self) -> Registro:  # noqa: PYI034
        self.caminho.parent.mkdir(parents=True, exist_ok=True)
        self._arquivo = self.caminho.open("w", encoding="utf-8")
        return self

    def escrever(self, obj: dict[str, Any]) -> None:
        self._arquivo.write(json.dumps(obj, ensure_ascii=False) + "\n")
        if obj.get("tipo") == "linha":
            self.linhas += 1

    def __exit__(self, *exc: object) -> None:
        self._arquivo.close()


def ler_traco(caminho: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    cabecalho: dict[str, Any] = {}
    linhas: list[dict[str, Any]] = []
    with caminho.open(encoding="utf-8") as f:
        for raw in f:
            obj = json.loads(raw)
            if obj.get("tipo") == "cabecalho":
                cabecalho = obj
            elif obj.get("tipo") == "linha":
                linhas.append(obj)
    return cabecalho, linhas


# ---------------------------------------------------------------- virtual


def argumentos_virtuais(binario: Path, params: dict[str, Any]) -> list[str]:
    args = [str(binario), "pesquisa", "--seed", str(params["seed"])]
    for chave in sorted(params):
        if chave in ("seed", "alvo", "alvo_uuid") or chave in PARAMETROS_FIRMWARE:
            continue
        args += ["--param", f"{chave}={params[chave]}"]
    return args


def coletar_virtual(binario: Path, execucao: Execucao, destino: Path, timeout_s: float = 900) -> dict[str, Any]:
    """Executa uma execução na bancada virtual (rápida e determinística dada a semente)."""
    params = com_sorteios(execucao.parametros)
    params.setdefault("alvo", ALVO_PADRAO)
    params.setdefault("alvo_uuid", UUID_PADRAO)
    args = argumentos_virtuais(binario, params)
    inicio = time.monotonic()
    proc = subprocess.run(args, capture_output=True, text=True, timeout=timeout_s, check=False)
    duracao = time.monotonic() - inicio
    config_linha = ""
    firmware = ""
    with Registro(destino / "traco.jsonl") as reg:
        reg.escrever({"tipo": "cabecalho", "execucao": execucao.id, "bancada": "virtual", "parametros": params,
                      "comando": args[1:], "registrado_em": _agora()})
        for raw in proc.stdout.splitlines():
            if m := STAMP.match(raw):
                linha = m.group(2)
                reg.escrever({"tipo": "linha", "fonte": "probe", "t_ref_ms": float(m.group(1)), "t_host_ns": None,
                              "linha": linha})
                if "[GW] CONFIG " in linha and not config_linha:
                    config_linha = linha
                if "FIRMWARE=" in linha and not firmware:
                    firmware = linha
            elif m := MARK.match(raw):
                reg.escrever({"tipo": "linha", "fonte": "bancada", "t_ref_ms": float(m.group(1)), "t_host_ns": None,
                              "linha": "pesquisa " + m.group(2)})
        linhas = reg.linhas
    status = "ok" if proc.returncode == 0 and linhas > 0 and "janela=FIM" in proc.stdout else "erro"
    return {
        "status": status, "codigo_saida": proc.returncode, "erro": proc.stderr[-2000:] if status != "ok" else "",
        "linhas": linhas, "duracao_s": round(duracao, 3), "config_firmware": parametros_config(config_linha),
        "firmware": firmware,
    }


# ----------------------------------------------------------------- física


class Porta(Protocol):
    def write(self, data: bytes) -> int: ...
    def read(self, size: int) -> bytes: ...
    @property
    def in_waiting(self) -> int: ...


class LeitorSerial:
    """Lê linhas da Probe numa thread, carimbando cada uma na chegada."""

    def __init__(self, porta: Porta, t0_ns: int) -> None:
        self.porta = porta
        self.t0_ns = t0_ns
        self.linhas: list[tuple[int, str]] = []
        self._buffer = bytearray()
        self._parar = threading.Event()
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._laco, daemon=True)

    def iniciar(self) -> None:
        self._thread.start()

    def parar(self) -> None:
        self._parar.set()
        self._thread.join(timeout=2)

    def _laco(self) -> None:
        while not self._parar.is_set():
            try:
                n = self.porta.in_waiting
                dados = self.porta.read(n or 1)
            except OSError:
                break
            agora = time.monotonic_ns()
            if not dados:
                time.sleep(0.002)
                continue
            self._buffer.extend(dados)
            while b"\n" in self._buffer:
                raw, _, resto = bytes(self._buffer).partition(b"\n")
                self._buffer = bytearray(resto)
                texto = raw.decode("utf-8", "replace").strip()
                if texto:
                    with self._lock:
                        self.linhas.append((agora, texto))

    def copia(self) -> list[tuple[int, str]]:
        with self._lock:
            return list(self.linhas)

    def tem(self, trecho: str, desde: int = 0) -> bool:
        with self._lock:
            return any(trecho in t for _, t in self.linhas[desde:])


def coletar_fisica(porta: Porta, execucao: Execucao, destino: Path,
                   operador: Callable[[str], None] = lambda msg: input(f"\n>>> {msg} [Enter] "),
                   relogio: Callable[[], float] = time.monotonic, dormir: Callable[[float], None] = time.sleep,
                   ) -> dict[str, Any]:
    """Executa o protocolo na Probe 00 real. Os sensores dos alvos já devem estar associados."""
    params = com_sorteios(execucao.parametros)
    alvo = str(params.get("alvo", ALVO_PADRAO))
    node = alvo.split(".")[0]
    t0_ns = time.monotonic_ns()
    leitor = LeitorSerial(porta, t0_ns)
    eventos: list[tuple[int, str, str]] = []  # (t_ns, fonte, linha) do coletor

    def enviar(cmd: str) -> None:
        porta.write((cmd + "\n").encode("ascii"))
        eventos.append((time.monotonic_ns(), "coletor", f"comando {cmd}"))

    def marca(texto: str) -> None:
        eventos.append((time.monotonic_ns(), "coletor", f"pesquisa {texto}"))

    def esperar(segundos: float, condicao: Callable[[], bool] | None = None) -> None:
        fim = relogio() + segundos
        while relogio() < fim:
            if condicao and condicao():
                return
            dormir(0.05)

    leitor.iniciar()
    inicio = relogio()
    try:
        marca("parametros " + " ".join(f"{k}={params[k]}" for k in sorted(params)))
        enviar("PROBE_VERSION")
        enviar("CONFIG")
        estado = "ON" if params.get("iot", "on") != "off" else "OFF"
        if int(params.get("sensors", 1)) > 0:
            enviar(f"CMD TARGET={alvo} ACTION=TELEMETRY STATE={estado} TX=pesquisa")
        # Aquecimento mais a fase sorteada (phase_ms), como na bancada virtual.
        esperar(float(params.get("warmup_s", 10)) + float(params.get("phase_ms", 0)) / 1000.0)
        marca("janela=INICIO")
        t_janela = relogio()
        duracao = float(params.get("duration_s", 60))
        if int(params.get("load_pct", 0)) > 0:
            enviar(f"FAULT LOAD {params['load_pct']} {int(duracao)} {params.get('load_prio', 'HIGH')} "
                   f"{params.get('load_seed', params.get('seed', 0))}")
        ecos = comandos_eco({**params, "alvo": alvo})
        fase_eco = "LOCAL" if ecos else "FIM"
        marca_eco = len(leitor.copia())
        if ecos:
            enviar(ecos[0])
        falha = params.get("fault", "none")
        falha_em = float(params.get("fault_at_s", 10))
        falha_s = float(params.get("fault_s", 20))
        falha_iniciada = falha == "none"
        falha_terminada = falha in ("none", "owner_power", "ap_outage", "drop_session")
        while relogio() - t_janela < duracao:
            dormir(0.05)
            decorrido = relogio() - t_janela
            if fase_eco == "LOCAL" and leitor.tem(f"ECHO_DONE node={node}", marca_eco):
                fase_eco = "FIM"
                if ecos and int(params.get("sensors", 1)) > 0:
                    enviar(ecos[1])
            if not falha_iniciada and decorrido >= falha_em:
                falha_iniciada = True
                if falha == "sensor_off":
                    operador(f"Desligue agora o sensor {params.get('alvo_uuid', alvo)} e tecle Enter no mesmo instante")
                    marca(f"verdade=SENSOR_OFF uuid={params.get('alvo_uuid', '')}")
                elif falha == "owner_power":
                    enviar(f"FAULT POWER_CYCLE {node} {int(falha_s)}")
                elif falha == "ap_outage":
                    enviar(f"FAULT AP_OUTAGE {node} {int(falha_s)}")
                elif falha == "drop_session":
                    enviar(f"FAULT DROP_SESSION {node}")
            if falha_iniciada and not falha_terminada and decorrido >= falha_em + falha_s:
                falha_terminada = True
                operador(f"Religue agora o sensor {params.get('alvo_uuid', alvo)} e tecle Enter no mesmo instante")
                marca(f"verdade=SENSOR_ON uuid={params.get('alvo_uuid', '')}")
        enviar("ECHO CANCEL")
        esperar(0.5)
        marca("janela=FIM")
        if estado == "OFF" and int(params.get("sensors", 1)) > 0:
            enviar(f"CMD TARGET={alvo} ACTION=TELEMETRY STATE=ON TX=pesquisa")
            esperar(0.3)
    finally:
        leitor.parar()
    todas = [(t, "probe", texto) for t, texto in leitor.copia()] + eventos
    todas.sort(key=lambda x: x[0])
    config_linha = next((t for _, _, t in todas if "[GW] CONFIG " in t), "")
    firmware = next((t for _, _, t in todas if "FIRMWARE=" in t), "")
    with Registro(destino / "traco.jsonl") as reg:
        reg.escrever({"tipo": "cabecalho", "execucao": execucao.id, "bancada": "fisica", "parametros": params,
                      "registrado_em": _agora()})
        for t_ns, fonte, texto in todas:
            reg.escrever({"tipo": "linha", "fonte": fonte, "t_ref_ms": (t_ns - t0_ns) / 1e6, "t_host_ns": t_ns,
                          "linha": texto})
        linhas = reg.linhas
    return {"status": "ok" if config_linha else "erro",
            "erro": "" if config_linha else "a Probe não respondeu ao comando CONFIG (firmware anterior à 0.21?)",
            "linhas": linhas, "duracao_s": round(relogio() - inicio, 3),
            "config_firmware": parametros_config(config_linha), "firmware": firmware}


# -------------------------------------------------------------- campanha


def compilar_bancada(destino: Path, firmware: dict[str, int], progresso: Callable[[str], None] = print) -> Path:
    """Bancada virtual compilada com parâmetros de firmware próprios (reaproveitada se já existir)."""
    binario = destino / "sim_network"
    if binario.exists():
        return binario
    destino.mkdir(parents=True, exist_ok=True)
    flags = " ".join(f"-D{k}={v}" for k, v in sorted(firmware.items()))
    progresso(f"compilando a bancada virtual com {flags or 'os parâmetros padrão'}")
    script = raiz_repositorio() / "Codigo/scripts/build_network_sim.sh"
    proc = subprocess.run([str(script), str(binario)], env={**os.environ, "IOC_SIM_CXXFLAGS": flags},
                          capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"falha ao compilar a bancada virtual: {proc.stderr[-1500:]}")
    (destino / "parametros.json").write_text(json.dumps(firmware, indent=2) + "\n", encoding="utf-8")
    return binario


def conferir_firmware(params: dict[str, Any], config: dict[str, str]) -> str:
    """Erro (texto) quando a Probe não roda a compilação pedida pelo desenho."""
    divergencias = []
    for chave, (define, campo) in PARAMETROS_FIRMWARE.items():
        if chave in params and str(config.get(campo)) != str(int(params[chave])):
            divergencias.append(f"{define}: pedido {params[chave]}, Probe informa {config.get(campo)}")
    return "; ".join(divergencias)


def preparar(desenho: dict[str, Any], saida: Path) -> tuple[Path, dict[str, Any], dict[str, Any]]:
    base = saida / desenho.get("nome", "sem_nome")
    base.mkdir(parents=True, exist_ok=True)
    p = plano(desenho)
    (base / "desenho.json").write_text(json.dumps(desenho, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (base / "plano.json").write_text(json.dumps(p, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    amb_path = base / "ambiente.json"
    amb = ambiente()
    if amb_path.exists():
        anterior = json.loads(amb_path.read_text(encoding="utf-8"))
        if anterior.get("fontes_firmware_sha256") != amb["fontes_firmware_sha256"]:
            # Mesma campanha com outro firmware não pode ser misturada.
            raise RuntimeError("as fontes do firmware mudaram desde o início desta campanha; use outra pasta de saída")
        amb = anterior
    else:
        amb_path.write_text(json.dumps(amb, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return base, p, amb


def executar(desenho: dict[str, Any], saida: Path, *, binario: Path | None = None, porta: Porta | None = None,
             paralelo: int = 1, limite: int | None = None, progresso: Callable[[str], None] = print,
             operador: Callable[[str], None] | None = None) -> dict[str, int]:
    """Executa as execuções pendentes do plano (na ordem sorteada)."""
    base, _plano, amb = preparar(desenho, saida)
    execucoes = sorted(expandir(desenho), key=lambda e: e.ordem)
    pendentes = []
    for e in execucoes:
        manifesto = base / "execucoes" / e.id / "manifesto.json"
        if manifesto.exists() and json.loads(manifesto.read_text(encoding="utf-8")).get("status") == "ok":
            continue
        pendentes.append(e)
    if limite is not None:
        pendentes = pendentes[:limite]
    contagem = {"ok": 0, "erro": 0, "ja_feitas": len(execucoes) - len(pendentes) if limite is None else 0}

    def uma(e: Execucao) -> str:
        destino = base / "execucoes" / e.id
        destino.mkdir(parents=True, exist_ok=True)
        inicio = _agora()
        if binario is not None:
            fw = firmware_de(e.parametros)
            exe = binario if not fw else compilar_bancada(
                base / "binarios" / sha256_json(fw)[:12], fw, progresso)
            resultado = coletar_virtual(exe, e, destino)
        elif porta is not None:
            resultado = coletar_fisica(porta, e, destino, **({"operador": operador} if operador else {}))
        else:
            raise ValueError("informe o binário da bancada virtual ou a porta da Probe 00")
        erro_fw = conferir_firmware(e.parametros, resultado.get("config_firmware", {}))
        if erro_fw:
            resultado.update(status="erro", erro=f"firmware diferente do pedido: {erro_fw}")
        manifesto = {
            "execucao": e.id, "bloco": e.bloco, "condicao": e.condicao, "repeticao": e.repeticao,
            "semente": e.semente, "ordem": e.ordem, "parametros": e.parametros,
            "bancada": "virtual" if binario is not None else "fisica", "inicio": inicio, "fim": _agora(),
            "desenho_sha256": sha256_json(desenho),
            "fontes_firmware_sha256": amb["fontes_firmware_sha256"], "git": amb["git"],
            "hash_configuracao": hash_configuracao(parametros_sem_semente(e.parametros),
                                                   resultado.get("config_firmware", {}),
                                                   amb["fontes_firmware_sha256"]),
            **resultado,
        }
        (destino / "manifesto.json").write_text(json.dumps(manifesto, indent=2, ensure_ascii=False) + "\n",
                                                encoding="utf-8")
        return resultado["status"]

    if binario is not None:
        for fw in {sha256_json(firmware_de(e.parametros)): firmware_de(e.parametros) for e in pendentes}.values():
            if fw:
                compilar_bancada(base / "binarios" / sha256_json(fw)[:12], fw, progresso)
    if binario is not None and paralelo > 1:
        with ThreadPoolExecutor(max_workers=paralelo) as pool:
            for e, status in zip(pendentes, pool.map(uma, pendentes)):
                contagem[status] = contagem.get(status, 0) + 1
                progresso(f"{e.id}: {status}")
    else:
        for e in pendentes:
            status = uma(e)
            contagem[status] = contagem.get(status, 0) + 1
            progresso(f"{e.id}: {status}")
    return contagem
