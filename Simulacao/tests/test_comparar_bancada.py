"""Comparação dos tempos da gravação física com os da bancada virtual (Simulacao/comparar_bancada.py)."""
from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Simulacao"))

import comparar_bancada as cb  # noqa: E402

UUID = "0xE6616408432B6F39"


def assoc(t: int, node: int, state: str, session: str, uuid: str = UUID) -> str:
    return (f"@{t} [GW] WIRELESS_ASSOC node={node} child=1 uuid={uuid} profile=VIBRATION state={state} "
            f"rssi=-50 protocol=6 failover=AUTO session={session} t_us={t * 1000}")


def heartbeat(t: int, leader: int) -> str:
    return f"@{t} [GW] HEARTBEAT lider={leader} rodada=1 sensores_ativos=1 modo=3 periodo=1000 ms t_us={t * 1000}"


def failover(t: int, to: int, uuid: str = UUID) -> str:
    return f"@{t} [GW] WIRELESS_FAILOVER uuid={uuid} from=1 to={to} reason=OWNER_LOST rssi=-68 t_us={t * 1000}"


def recording(*, leader_gap: int, reassoc: int, opening: int) -> str:
    """Uma sessão com uma abertura de canal, uma troca de líder e uma reassociação de duração conhecida."""
    lines = [
        "# iot-over-can: sessão gravada em 2026-11-17T10:15:00-03:00, porta /dev/ttyUSB0, modo=serial",
        assoc(1000, 1, "ASSOCIATING", "NONE"),
        assoc(1000 + opening, 1, "ONLINE", "SECURE"),
        "# t=5000 comando: WIRELESS BIND 1 0xE6616408432B6F39 AUTO",
        heartbeat(10000, 3),
        heartbeat(11000, 3),
        heartbeat(11000 + leader_gap, 2),
        failover(30000, 2),
        assoc(30001, 2, "ASSOCIATING", "NONE"),
        assoc(30000 + reassoc, 2, "ONLINE", "SECURE"),
    ]
    return "\n".join(lines) + "\n"


def run(argv: list[str]) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = cb.main(argv)
        except SystemExit as exit_:   # argparse
            code = int(exit_.code or 0)
    return code, out.getvalue(), err.getvalue()


class ExtractionTest(unittest.TestCase):
    def lines(self, text: str) -> list[cb.Line]:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "gravacao.log"
            path.write_text(text, encoding="utf-8")
            return cb.read_lines(path)

    def test_measures_the_three_intervals(self) -> None:
        measured = cb.measure(self.lines(recording(leader_gap=7500, reassoc=19000, opening=1800)))
        self.assertEqual(measured["troca_de_lider_ms"], [7500])
        self.assertEqual(measured["reassociacao_ms"], [19000])
        self.assertEqual(measured["abertura_do_canal_ms"], [1800])

    def test_comments_commands_and_garbage_are_ignored(self) -> None:
        lines = self.lines("# comentário\n# t=10 comando: CANAUTH\nsem instante\n@5 [GW] HEARTBEAT lider=1\n")
        self.assertEqual([line.t_ms for line in lines], [5])

    def test_a_reassociation_is_not_a_channel_opening(self) -> None:
        measured = cb.measure(self.lines(recording(leader_gap=7500, reassoc=19000, opening=1800)))
        self.assertEqual(len(measured["abertura_do_canal_ms"]), 1)   # a sessão do módulo 2 é reassociação

    def test_a_failover_waits_for_the_chosen_node_not_the_old_one(self) -> None:
        text = "\n".join([
            assoc(1000, 1, "ASSOCIATING", "NONE"),
            assoc(2000, 1, "ONLINE", "SECURE"),
            failover(10000, 2),
            assoc(12000, 1, "ONLINE", "SECURE"),   # o módulo antigo ainda reportando: não é a resposta
            assoc(30000, 2, "ONLINE", "SECURE"),
        ])
        self.assertEqual(cb.measure(self.lines(text))["reassociacao_ms"], [20000])

    def test_a_failover_without_a_secure_session_gives_no_measure(self) -> None:
        text = "\n".join([failover(10000, 2), assoc(12000, 2, "ASSOCIATING", "NONE")])
        self.assertEqual(cb.measure(self.lines(text))["reassociacao_ms"], [])

    def test_two_sensors_are_followed_separately(self) -> None:
        other = "0xE66164084327A12C"
        text = "\n".join([
            assoc(1000, 1, "ASSOCIATING", "NONE"),
            assoc(1500, 1, "ASSOCIATING", "NONE", other),
            assoc(2800, 1, "ONLINE", "SECURE"),
            assoc(4500, 1, "ONLINE", "SECURE", other),
        ])
        self.assertEqual(cb.measure(self.lines(text))["abertura_do_canal_ms"], [1800, 3000])

    def test_leader_changes_are_counted_each_time(self) -> None:
        text = "\n".join([heartbeat(1000, 3), heartbeat(2000, 3), heartbeat(9000, 2), heartbeat(10000, 2),
                           heartbeat(15000, 3)])
        self.assertEqual(cb.measure(self.lines(text))["troca_de_lider_ms"], [7000, 5000])


class ToleranceTest(unittest.TestCase):
    def test_the_larger_of_absolute_and_relative_applies(self) -> None:
        self.assertEqual(cb.tolerance_ms(1800, 1500, 0.25), 1500)    # valor pequeno: vale o absoluto
        self.assertEqual(cb.tolerance_ms(19000, 1500, 0.25), 4750)   # valor grande: vale o relativo

    def test_inside_and_outside(self) -> None:
        bench = {"reassociacao_ms": [19000]}
        self.assertEqual(cb.compare(bench, {"reassociacao_ms": [23700]}).rows[0].verdict, "OK")     # +4700 <= 4750
        self.assertEqual(cb.compare(bench, {"reassociacao_ms": [23800]}).rows[0].verdict, "FORA")   # +4800
        self.assertEqual(cb.compare(bench, {"reassociacao_ms": [14300]}).rows[0].verdict, "OK")     # -4700
        self.assertEqual(cb.compare(bench, {"reassociacao_ms": [14200]}).rows[0].verdict, "FORA")   # -4800

    def test_a_missing_side_is_reported_not_guessed(self) -> None:
        result = cb.compare({"reassociacao_ms": [19000, 21000]}, {"reassociacao_ms": [19500]})
        self.assertEqual([row.verdict for row in result.rows], ["OK", "SEM_DADO"])
        self.assertTrue(result.notes)
        self.assertEqual(result.result(require_all=False), "OK")
        self.assertEqual(result.result(require_all=True), "FORA")

    def test_nothing_in_common(self) -> None:
        result = cb.compare({"reassociacao_ms": [19000]}, {"troca_de_lider_ms": [7000]})
        self.assertEqual(result.result(require_all=False), "SEM_DADOS")


class CommandLineTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)

    def write(self, name: str, text: str) -> Path:
        path = self.dir / name
        path.write_text(text, encoding="utf-8")
        return path

    def test_a_recording_close_to_the_bench_passes(self) -> None:
        bench = self.write("bancada.log", recording(leader_gap=7500, reassoc=19000, opening=1800))
        physical = self.write("fisica.log", recording(leader_gap=8600, reassoc=22000, opening=2600))
        code, out, _ = run([str(physical), "--bancada", str(bench)])
        self.assertEqual(code, 0, out)
        self.assertIn("COMPARACAO RESULT=OK medidas=3 fora=0 sem_dado=0", out)

    def test_a_recording_far_from_the_bench_fails_and_says_which(self) -> None:
        bench = self.write("bancada.log", recording(leader_gap=7500, reassoc=19000, opening=1800))
        physical = self.write("fisica.log", recording(leader_gap=7600, reassoc=31000, opening=1900))
        code, out, _ = run([str(physical), "--bancada", str(bench)])
        self.assertEqual(code, 1, out)
        self.assertIn("COMPARACAO RESULT=FORA", out)
        row = [line for line in out.splitlines() if line.startswith("reassociacao_ms")][0]
        self.assertTrue(row.rstrip().endswith("FORA"), row)

    def test_the_tolerance_can_be_widened(self) -> None:
        bench = self.write("bancada.log", recording(leader_gap=7500, reassoc=19000, opening=1800))
        physical = self.write("fisica.log", recording(leader_gap=7600, reassoc=31000, opening=1900))
        code, _, _ = run([str(physical), "--bancada", str(bench), "--tolerancia-relativa", "0.7"])
        self.assertEqual(code, 0)

    def test_without_common_events_the_exit_code_is_three(self) -> None:
        bench = self.write("bancada.log", recording(leader_gap=7500, reassoc=19000, opening=1800))
        physical = self.write("fisica.log", "# nada\n@10 [GW] BUS_STATS load=1\n")
        code, _, err = run([str(physical), "--bancada", str(bench)])
        self.assertEqual(code, 3)
        self.assertIn("nenhuma medida existe", err)

    def test_require_all_fails_when_the_recording_lacks_a_measure(self) -> None:
        bench = self.write("bancada.log", recording(leader_gap=7500, reassoc=19000, opening=1800))
        physical = self.write("fisica.log", "\n".join([assoc(1000, 1, "ASSOCIATING", "NONE"),
                                                       assoc(2800, 1, "ONLINE", "SECURE")]))
        code, out, _ = run([str(physical), "--bancada", str(bench)])
        self.assertEqual(code, 0, out)
        code, out, _ = run([str(physical), "--bancada", str(bench), "--exigir"])
        self.assertEqual(code, 1, out)

    def test_usage_errors(self) -> None:
        physical = self.write("fisica.log", recording(leader_gap=1, reassoc=1, opening=1))
        self.assertEqual(run([str(physical)])[0], 2)                                   # sem referência
        self.assertEqual(run([str(self.dir / "nao_existe.log"), "--cenario", "sensor"])[0], 2)
        self.assertEqual(run([str(physical), "--cenario", "nao_existe"])[0], 2)
        self.assertEqual(run([str(physical), "--cenario", "sensor", "--tolerancia-relativa", "-1"])[0], 2)

    def test_json_output(self) -> None:
        import json
        bench = self.write("bancada.log", recording(leader_gap=7500, reassoc=19000, opening=1800))
        code, out, _ = run([str(bench), "--bancada", str(bench), "--json"])
        self.assertEqual(code, 0)
        data = json.loads(out)
        self.assertEqual(data["result"], "OK")
        self.assertEqual(len(data["rows"]), 3)
        self.assertEqual({row["delta_ms"] for row in data["rows"]}, {0})


class CommittedReplaysTest(unittest.TestCase):
    """Os extratores dependem do texto das linhas da Probe: se ele mudar, estes testes avisam."""

    def measured(self, scenario: str) -> dict[str, list[int]]:
        return cb.measure(cb.read_lines(ROOT / "Front" / "replays" / f"{scenario}.log"))

    def test_owner_failover_has_one_reassociation_and_one_opening(self) -> None:
        measured = self.measured("owner_failover")
        self.assertEqual(len(measured["reassociacao_ms"]), 1)
        self.assertGreater(measured["reassociacao_ms"][0], 10000)
        self.assertEqual(len(measured["abertura_do_canal_ms"]), 1)

    def test_leader_failure_has_a_leader_change(self) -> None:
        gaps = self.measured("leader_failure")["troca_de_lider_ms"]
        self.assertGreaterEqual(len(gaps), 1)
        self.assertTrue(all(gap > 3000 for gap in gaps))

    def test_the_first_session_of_the_slow_radio_takes_longer_than_the_normal_one(self) -> None:
        slow = self.measured("first_session_slow")["abertura_do_canal_ms"]
        normal = self.measured("sensor")["abertura_do_canal_ms"]
        self.assertTrue(slow and normal)
        self.assertGreater(min(slow), 5 * max(normal))

    def test_a_replay_compared_with_itself_is_always_ok(self) -> None:
        for scenario in ("owner_failover", "leader_failure", "sensor", "two_sensors", "first_session_slow"):
            path = ROOT / "Front" / "replays" / f"{scenario}.log"
            code, out, _ = run([str(path), "--cenario", scenario])
            self.assertEqual(code, 0, f"{scenario}: {out}")


if __name__ == "__main__":
    unittest.main()
