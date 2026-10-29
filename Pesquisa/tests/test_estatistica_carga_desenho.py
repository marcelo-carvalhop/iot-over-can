"""Estatística, réplica do gerador de carga e expansão do desenho."""
from __future__ import annotations

import math
import re
import sys
import unittest
from pathlib import Path

RAIZ = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(RAIZ / "Pesquisa"))

from iocpesquisa import carga, desenho
from iocpesquisa import estatistica as st


class EstatisticaTest(unittest.TestCase):
    def test_percentis_como_o_numpy(self) -> None:
        xs = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
        # numpy.percentile(xs, q) (interpolação linear)
        self.assertAlmostEqual(st.percentile(xs, 50), 5.5)
        self.assertAlmostEqual(st.percentile(xs, 95), 9.55)
        self.assertAlmostEqual(st.percentile(xs, 99), 9.91)
        self.assertEqual(st.percentile([3.0], 99), 3.0)
        self.assertTrue(math.isnan(st.percentile([], 50)))

    def test_intervalos(self) -> None:
        self.assertAlmostEqual(st.t95(1), 12.706)
        self.assertAlmostEqual(st.t95(29), 2.045)
        self.assertAlmostEqual(st.t95(60), 2.000, places=2)
        lo, hi = st.ci_mean([10, 12, 14, 16, 18])
        self.assertAlmostEqual((lo + hi) / 2, 14.0)
        self.assertAlmostEqual(hi - 14.0, 2.776 * math.sqrt(10) / math.sqrt(5), places=3)
        # Wilson: 0 perdas em 100 -> limite superior ~3,7 %.
        p, lo, hi = st.wilson(0, 100)
        self.assertEqual(p, 0.0)
        self.assertAlmostEqual(lo, 0.0)
        self.assertAlmostEqual(hi, 0.0370, places=3)
        p, lo, hi = st.wilson(5, 10)
        self.assertAlmostEqual(lo, 0.2366, places=3)
        self.assertAlmostEqual(hi, 0.7634, places=3)
        # Bootstrap com semente fixa: reprodutível.
        dados = [1.0, 2.0, 2.5, 3.0, 7.0, 8.0]
        self.assertEqual(st.bootstrap_ci(dados), st.bootstrap_ci(dados))

    def test_tamanhos_de_efeito(self) -> None:
        a = [10.0, 11.0, 12.0, 13.0, 14.0]
        b = [20.0, 21.0, 22.0, 23.0, 24.0]
        g, lo, hi = st.hedges_g(a, b)
        self.assertGreater(g, 5)
        self.assertLess(lo, g)
        self.assertGreater(hi, g)
        self.assertEqual(st.cliffs_delta(a, b), (1.0, "grande"))
        self.assertEqual(st.cliffs_delta(b, a)[0], -1.0)
        self.assertEqual(st.cliffs_delta(a, a)[1], "desprezível")
        _, p = st.mann_whitney(a, b)
        self.assertLess(p, 0.02)
        _, p_igual = st.mann_whitney(a, a)
        self.assertGreater(p_igual, 0.9)
        # Holm: [0.01, 0.04, 0.03] -> [0.03, 0.06, 0.06]
        self.assertEqual([round(x, 4) for x in st.holm([0.01, 0.04, 0.03])], [0.03, 0.06, 0.06])


class CargaTest(unittest.TestCase):
    def test_sequencia_igual_a_do_firmware(self) -> None:
        # As gravações da bancada virtual trazem o seq_hash calculado pela Probe.
        replay = (RAIZ / "Front/replays/bus_load.log").read_text(encoding="utf-8")
        fins = re.findall(r"kind=BUS_LOAD state=ENDED .*?priority=(\w+) frames=(\d+) .*?seed=(\d+) seq_hash=(0x[0-9A-F]+)",
                          replay)
        self.assertGreaterEqual(len(fins), 2)
        for prio, quadros, semente, h in fins:
            self.assertEqual(carga.sequence_hash(int(semente), prio, int(quadros)), int(h, 16))

    def test_semente_muda_o_conteudo_e_mix_sorteia_as_classes(self) -> None:
        a = list(carga.frames(7, "MIX", 300))
        b = list(carga.frames(7, "MIX", 300))
        c = list(carga.frames(8, "MIX", 300))
        self.assertEqual(a, b)
        self.assertNotEqual(a, c)
        ids = {i for i, _ in a}
        self.assertEqual(ids, {carga.ID_HIGH, carga.ID_MID, carga.ID_LOW})
        self.assertEqual({i for i, _ in carga.frames(0, "LOW", 5)}, {carga.ID_LOW})
        # O contador de 16 bits dá a volta.
        ultimo = list(carga.frames(1, "HIGH", 65538))[-1][1]
        self.assertEqual(ultimo[:2], bytes((0, 1)))
        with self.assertRaises(ValueError):
            list(carga.frames(1, "TOP", 1))

    def test_duracao_de_quadro(self) -> None:
        # Quadro de 8 bytes de zeros: 111 bits sem preenchimento + inserções + 13 fixos.
        self.assertGreater(carga.frame_bits(0x010, bytes(8)), 111)
        self.assertLessEqual(carga.frame_bits(0x7EF, bytes([0x55] * 8)), 135)


class DesenhoTest(unittest.TestCase):
    def test_desenhos_versionados_sao_validos(self) -> None:
        for arq in sorted((RAIZ / "Pesquisa/desenhos").glob("*.json")):
            with self.subTest(arq=arq.name):
                d = desenho.carregar(arq)
                p = desenho.plano(d)
                self.assertGreater(len(p["execucoes"]), 0)
                ids = [e["id"] for e in p["execucoes"]]
                self.assertEqual(len(ids), len(set(ids)))

    def test_expansao_exclusoes_sementes_e_ordem(self) -> None:
        d = {
            "nome": "t", "semente_base": 3, "blocos": [{
                "nome": "b", "repeticoes": 4,
                "fatores": {"load_pct": [0, 50], "load_prio": ["HIGH", "LOW"]},
                "fixos": {"duration_s": 10},
                "excluir": [{"load_pct": 0, "load_prio": "LOW"}],
            }],
        }
        execs = desenho.expandir(d)
        self.assertEqual(len(execs), 3 * 4)  # (0,HIGH), (50,HIGH), (50,LOW)
        self.assertNotIn("load_pct=0,load_prio=LOW", {e.condicao for e in execs})
        self.assertEqual([e.semente for e in execs], [e.semente for e in desenho.expandir(d)])
        self.assertEqual(sorted(e.ordem for e in execs), list(range(len(execs))))
        self.assertNotEqual([e.ordem for e in execs], list(range(len(execs))))  # ordem sorteada
        self.assertEqual(len({e.semente for e in execs}), len(execs))
        for e in execs:
            self.assertEqual(e.parametros["load_seed"], e.semente)
            self.assertEqual(e.parametros["duration_s"], 10)
        outro = desenho.expandir({**d, "semente_base": 4})
        self.assertNotEqual([e.semente for e in execs], [e.semente for e in outro])

    def test_validacao(self) -> None:
        base = {"nome": "t", "blocos": [{"nome": "b", "repeticoes": 1, "fatores": {"load_pct": [0]}}]}
        desenho.validar(base)
        for ruim in (
            {**base, "blocos": []},
            {**base, "bancada": "lua"},
            {**base, "blocos": [{"nome": "b", "repeticoes": 0}]},
            {**base, "blocos": [{"nome": "b", "repeticoes": 1, "fatores": {"velocidade": [1]}}]},
            {**base, "blocos": [{"nome": "b", "repeticoes": 1, "fatores": {"fault": ["meteoro"]}}]},
            {**base, "comparacoes": [{"fator": "iot"}]},
        ):
            with self.assertRaises(ValueError):
                desenho.validar(ruim)
        self.assertEqual(desenho.firmware_de({"fw_assoc_stale_ms": "10000", "load_pct": 5}),
                         {"IOT_ASSOC_STALE_MS": 10000})


if __name__ == "__main__":
    unittest.main()
