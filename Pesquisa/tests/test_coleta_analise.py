"""Coleta (física, com uma Probe de mentira; virtual, com a bancada real) e análise."""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(RAIZ / "Pesquisa"))

from iocpesquisa import carga, coleta, desenho, metricas, relatorio
from iocpesquisa.desenho import Execucao


class ProbeFalsa:
    """O lado da Probe 00 no protocolo serial, o suficiente para o coletor."""

    def __init__(self) -> None:
        self.comandos: list[str] = []
        self._saida = bytearray()
        self._entrada = b""
        self._lock = threading.Lock()
        self.t_us = 1_000_000

    def _emitir(self, linha: str) -> None:
        self.t_us += 1000
        with self._lock:
            self._saida.extend((linha.replace("{t}", str(self.t_us)) + "\n").encode())

    @property
    def in_waiting(self) -> int:
        with self._lock:
            return len(self._saida)

    def read(self, n: int) -> bytes:
        with self._lock:
            dados = bytes(self._saida[:n])
            del self._saida[:n]
        return dados

    def write(self, dados: bytes) -> int:
        self._entrada += dados
        while b"\n" in self._entrada:
            linha, _, self._entrada = self._entrada.partition(b"\n")
            self._comando(linha.decode())
        return len(dados)

    def _comando(self, cmd: str) -> None:
        self.comandos.append(cmd)
        partes = cmd.split()
        if cmd == "CONFIG":
            self._emitir("[GW] CONFIG node=0 bitrate=500000 hb_period_ms=1000 assoc_stale_ms=15000 assoc_lost_ms=45000 "
                         "assoc_hysteresis_ms=0 stream_period_ms=1000 t_us={t}")
        elif cmd == "PROBE_VERSION":
            self._emitir("PROBE_VERSION FIRMWARE=0.21.0 PROTOCOL=CAN_CLASSIC_V1 NODE=0")
        elif partes[:2] == ["FAULT", "LOAD"]:
            pct, _dur, prio, semente = partes[2], partes[3], partes[4], int(partes[5])
            quadros = 1234
            h = carga.sequence_hash(semente, prio, quadros)
            self._emitir(f"[GW] FAULT_EVENT node=0 kind=BUS_LOAD state=STARTED duration_ms=20000 seq=1 load_pct={pct} "
                         f"priority={prio} frames=0 achieved_pct=0.00 seed={semente} seq_hash=0x811C9DC5 t_us={{t}}")
            self._emitir(f"[GW] FAULT_EVENT node=0 kind=BUS_LOAD state=ENDED duration_ms=20000 seq=1 load_pct={pct} "
                         f"priority={prio} frames={quadros} achieved_pct={pct}.00 seed={semente} seq_hash=0x{h:08X} t_us={{t}}")
        elif partes[0] == "ECHO" and partes[1] != "CANCEL":
            alvo, n, caminho = partes[1], int(partes[2]), partes[4]
            node, child = alvo.split(".")
            for i in range(n):
                wl = 9000 if caminho == "SENSOR" else -1
                self._emitir(f"[GW] ECHO node={node} child={child} path={caminho} n={i + 1} seq={i} status=APPLIED "
                             f"rtt_us={1500 + 100 * i + (11000 if caminho == 'SENSOR' else 0)} wl_us={wl} t_us={{t}}")
            self._emitir(f"[GW] ECHO_DONE node={node} child={child} path={caminho} sent={n} received={n} lost=0 t_us={{t}}")
        elif partes[:2] == ["FAULT", "POWER_CYCLE"]:
            self._emitir(f"[GW] FAULT_TX kind=POWER_CYCLE node={partes[2]} duration_ms={int(partes[3]) * 1000} seq=2 t_us={{t}}")
            self._emitir(f"[GW] FAULT_EVENT node={partes[2]} kind=POWER_CYCLE state=STARTED "
                         f"duration_ms={int(partes[3]) * 1000} seq=2 t_us={{t}}")


class RelogioFalso:
    def __init__(self) -> None:
        self.t = 0.0

    def agora(self) -> float:
        return self.t

    def dormir(self, dt: float) -> None:
        self.t += dt
        time.sleep(0.0004)  # deixa a leitura da serial acompanhar


class ColetaFisicaTest(unittest.TestCase):
    def test_protocolo_na_probe(self) -> None:
        probe = ProbeFalsa()
        relogio = RelogioFalso()
        avisos: list[str] = []
        params = {"seed": 99, "load_seed": 99, "load_pct": 50, "load_prio": "HIGH", "iot": "off", "sensors": 1,
                  "alvo": "1.1", "warmup_s": 2, "duration_s": 20, "echo_n": 5, "echo_period_ms": 300,
                  "fault": "owner_power", "fault_at_s": 5, "fault_s": 8}
        e = Execucao("b-x-r001", "b", "c", 1, 99, 0, params)
        with tempfile.TemporaryDirectory() as tmp:
            r = coleta.coletar_fisica(probe, e, Path(tmp), operador=avisos.append,
                                      relogio=relogio.agora, dormir=relogio.dormir)
            self.assertEqual(r["status"], "ok", r)
            self.assertEqual(r["config_firmware"]["assoc_stale_ms"], "15000")
            s = desenho.sorteios(99)["echo_seed"]
            esperado = ["PROBE_VERSION", "CONFIG", "CMD TARGET=1.1 ACTION=TELEMETRY STATE=OFF TX=pesquisa",
                        "FAULT LOAD 50 20 HIGH 99", f"ECHO 1.1 5 300 LOCAL {s}", f"ECHO 1.1 5 300 SENSOR {s + 1}",
                        "FAULT POWER_CYCLE 1 8", "ECHO CANCEL", "CMD TARGET=1.1 ACTION=TELEMETRY STATE=ON TX=pesquisa"]
            self.assertEqual(probe.comandos, esperado)
            cab, regs = coleta.ler_traco(Path(tmp) / "traco.jsonl")
            self.assertEqual(cab["bancada"], "fisica")
            self.assertTrue(all(r["t_host_ns"] for r in regs))
            m = metricas.calcular(cab, regs)
            self.assertEqual(len(m["amostras"]["eco_local_us"]), 5)
            self.assertEqual(m["amostras"]["eco_wifi_us"], [9000.0] * 5)
            self.assertTrue(m["carga_sequencia_confere"])
            # A referência da falha veio do FAULT_EVENT da Probe; sem linhas de vínculo, nada foi detectado.
            self.assertNotIn("falha_sem_referencia", m)
            self.assertIn("deteccao_sessao_ms", m)
            self.assertIsNone(m["deteccao_sessao_ms"])
        self.assertEqual(avisos, [])  # owner_power não pede nada ao operador

    def test_eco_de_poisson_por_padrao_e_sorteios_fora_do_hash(self) -> None:
        params = {"seed": 5, "echo_n": 40, "echo_period_ms": 500, **desenho.sorteios(5)}
        local, sensor = coleta.comandos_eco(params)
        s = params["echo_seed"]
        self.assertEqual((local, sensor), (f"ECHO 1.1 40 500 LOCAL {s}", f"ECHO 1.1 40 500 SENSOR {s + 1}"))
        self.assertEqual(coleta.comandos_eco({**params, "echo_sampling": "periodic"}),
                         ("ECHO 1.1 40 500 LOCAL", "ECHO 1.1 40 500 SENSOR"))
        self.assertIsNone(coleta.comandos_eco({**params, "echo_n": 0}))
        # Repetições de uma condição só diferem nos sorteios: o hash de configuração é o mesmo.
        outra = {**params, "seed": 6, "load_seed": 6, **desenho.sorteios(6)}
        self.assertEqual(coleta.parametros_sem_semente(params), coleta.parametros_sem_semente(outra))

    def test_sorteios_repetidos_sao_pseudorreplicacao(self) -> None:
        def execucao(rep: int, **params: object) -> tuple[dict, dict]:
            return ({"bloco": "b", "condicao": "c", "parametros": params}, {})
        boas = [execucao(r, seed=r, echo_seed=100 + r, phase_ms=r * 7) for r in range(3)]
        self.assertEqual(relatorio.sorteios_repetidos(boas), [])
        repetidas = [execucao(r, seed=r, echo_seed=100, phase_ms=0) for r in range(3)]
        self.assertTrue(any("echo_seed repetida" in p for p in relatorio.sorteios_repetidos(repetidas)))
        antigas = [execucao(r, seed=r) for r in range(3)]
        self.assertEqual(len(relatorio.sorteios_repetidos(antigas)), 2)  # sem echo_seed e sem phase_ms

    def test_firmware_diferente_do_pedido_e_recusado(self) -> None:
        self.assertEqual(coleta.conferir_firmware({"fw_assoc_stale_ms": 15000}, {"assoc_stale_ms": "15000"}), "")
        erro = coleta.conferir_firmware({"fw_assoc_stale_ms": 10000}, {"assoc_stale_ms": "15000"})
        self.assertIn("IOT_ASSOC_STALE_MS", erro)


class MetricasTest(unittest.TestCase):
    def test_traco_sintetico(self) -> None:
        def linha(t: float, texto: str, fonte: str = "probe") -> dict:
            return {"tipo": "linha", "fonte": fonte, "t_ref_ms": t, "t_host_ns": None, "linha": texto}
        regs = [linha(0, "pesquisa janela=INICIO", "bancada")]
        for i in range(6):  # sinal de presença com uma variação de 2 ms
            regs.append(linha(100 + i * 1000, f"[GW] HEARTBEAT lider=3 rodada={i} sensores_ativos=3 modo=1 periodo=1000 ms "
                              f"t_us={1_000_000 * i + (2000 if i == 3 else 0)}"))
        for i, seq in enumerate((1, 2, 3, 6, 7)):  # duas amostras perdidas
            regs.append(linha(200 + i * 1000, f"TEL NODE=1 CHILD=1 SEQ={seq} RMS=0.1 T_US={1_000_000 * i}"))
        regs.append(linha(500, "[GW] ECHO node=1 child=1 path=LOCAL n=1 seq=1 status=APPLIED rtt_us=1500 wl_us=-1 t_us=1"))
        regs.append(linha(600, "[GW] ECHO node=1 child=1 path=LOCAL n=2 seq=2 status=TIMEOUT rtt_us=-1 wl_us=-1 t_us=2"))
        regs.append(linha(700, "[GW] WIRELESS_STATS node=1 child=1 crypto_rx_us=0 crypto_tx_us=41 crypto_max_us=60 "
                               "handshake_us=900 bridge_avg_us=-1 bridge_max_us=-1 t_us=3"))
        regs.append(linha(9000, "pesquisa janela=FIM", "bancada"))
        m = metricas.calcular({"parametros": {"fault": "none"}}, regs)
        self.assertEqual(m["tel_perdidas"], 2)
        self.assertEqual(m["tel_recebidas"], 5)
        self.assertEqual(m["eco_local_us_enviados"], 2)
        self.assertEqual(m["eco_local_us_perdidos"], 1)
        self.assertEqual(m["hb_desvio_max_us"], 2000.0)
        self.assertEqual(m["amostras"]["cripto_verificacao_us"], [0.0])  # zero é medida, -1 é ausência
        self.assertEqual(m["amostras"]["ponte_media_us"], [])
        self.assertEqual(m["transicoes_espurias"], 0)


@unittest.skipUnless(shutil.which("c++") and shutil.which("cc"), "compiladores C/C++ indisponíveis")
class PontaAPontaTest(unittest.TestCase):
    """A campanha de demonstração inteira na bancada virtual: coleta, análise e verificação."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binario = Path(cls.tmp.name) / "sim_network"
        subprocess.run([str(RAIZ / "Codigo/scripts/build_network_sim.sh"), str(cls.binario)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_campanha_de_demonstracao(self) -> None:
        d = desenho.carregar(RAIZ / "Pesquisa/desenhos/demonstracao.json")
        saida = Path(self.tmp.name) / "res"
        r = coleta.executar(d, saida, binario=self.binario, paralelo=4, progresso=lambda _m: None)
        self.assertEqual(r["ok"], 16)
        # Retomada: nada a refazer.
        self.assertEqual(coleta.executar(d, saida, binario=self.binario, progresso=lambda _m: None)["ja_feitas"], 16)
        base = saida / "demonstracao"
        res = relatorio.analisar(base, figuras=False)
        self.assertEqual(res["execucoes"], 16)
        self.assertGreater(res["comparacoes"], 5)
        problemas = [p for p in relatorio.verificar(base) if "alterações não gravadas" not in p]
        self.assertEqual(problemas, [])
        # O eco usa amostragem de Poisson, e as repetições diferem na fase e nos instantes das sondas.
        execs = relatorio.carregar_execucoes(base)[0]
        self.assertEqual({met.get("eco_amostragem") for m, met in execs if m["bloco"] == "desempenho"}, {"POISSON"})
        linhas = {(c["bloco"], c["condicao"]): c for c in relatorio.agregar(relatorio.carregar_execucoes(base)[0])}
        carga50 = linhas[("desempenho", "iot=on,load_pct=50")]
        self.assertAlmostEqual(carga50["carga_obtida_pct_media"], 50.0, delta=0.5)
        self.assertTrue(carga50["carga_sequencia_confere"])
        self.assertGreater(carga50["eco_local_us_n"], 20)
        # Sensor desligado: a sessão cai no prazo de 10 s do Node e o vínculo fica STALE 15 s depois.
        off = linhas[("falhas", "fault=sensor_off")]
        self.assertAlmostEqual(off["deteccao_sessao_ms_media"], 10000, delta=1500)
        self.assertAlmostEqual(off["deteccao_stale_ms_media"], 25000, delta=1500)
        # Queda do Node responsável: o líder decide em ~5 s e a telemetria volta por outro Node.
        dono = linhas[("falhas", "fault=owner_power")]
        self.assertLess(dono["decisao_reassociacao_ms_media"], 8000)
        self.assertLess(dono["retomada_telemetria_ms_media"], 40000)
        # O mesmo plano gera os mesmos registros (semente): refazer uma execução dá o mesmo traço.
        uma = min((base / "execucoes").iterdir())
        manifesto = json.loads((uma / "manifesto.json").read_text())
        e = Execucao(manifesto["execucao"], manifesto["bloco"], manifesto["condicao"], manifesto["repeticao"],
                     manifesto["semente"], manifesto["ordem"], manifesto["parametros"])
        outra = Path(self.tmp.name) / "refeita"
        coleta.coletar_virtual(self.binario, e, outra)
        linhas_a = [x for x in (uma / "traco.jsonl").read_text().splitlines()[1:]]
        linhas_b = [x for x in (outra / "traco.jsonl").read_text().splitlines()[1:]]
        self.assertEqual(linhas_a, linhas_b)


if __name__ == "__main__":
    unittest.main()
