"""O fluxo da bancada física (.github/workflows/bancada-fisica.yml) não roda na integração contínua comum:
estes testes só garantem que ele continua apontando para o que existe e que não dispara sozinho."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "bancada-fisica.yml"


class PhysicalBenchWorkflowTest(unittest.TestCase):
    def setUp(self) -> None:
        self.text = WORKFLOW.read_text(encoding="utf-8")

    def test_it_only_runs_on_request_and_only_on_the_lab_runner(self) -> None:
        self.assertIn("workflow_dispatch:", self.text)
        for trigger in ("push:", "pull_request:", "schedule:", "pull_request_target:"):
            self.assertNotIn(trigger, self.text)
        self.assertRegex(self.text, r"runs-on:\s*\[self-hosted,\s*bancada-fisica\]")
        self.assertNotIn("ubuntu-latest", self.text)

    def test_inputs_reach_the_shell_through_the_environment_only(self) -> None:
        # Uma entrada interpolada direto no script seria uma injeção de comando.
        for match in re.finditer(r"run:\s*\|(.*?)(?=\n      - |\Z)", self.text, re.S):
            self.assertNotIn("${{", match.group(1))
        self.assertIn("shell: bash", self.text)   # pipefail

    def test_every_script_it_calls_exists(self) -> None:
        paths = set(re.findall(r"(?:Codigo/scripts|Simulacao)/[\w./-]+\.py", self.text))
        self.assertGreaterEqual(len(paths), 2)
        for path in paths:
            self.assertTrue((ROOT / path).is_file(), path)

    def test_the_options_it_passes_are_options_of_those_scripts(self) -> None:
        smoke = (ROOT / "Codigo/scripts/smoke_boot.py").read_text(encoding="utf-8")
        for option in ("--porta", "--no", "--selftest", "--estrito"):
            self.assertIn(f'"{option}"', smoke)
            self.assertIn(option, self.text)
        compare = (ROOT / "Simulacao/comparar_bancada.py").read_text(encoding="utf-8")
        self.assertIn('"--cenario"', compare)

    def test_the_sensor_kinds_it_accepts_are_smoke_targets(self) -> None:
        smoke = (ROOT / "Codigo/scripts/smoke_boot.py").read_text(encoding="utf-8")
        match = re.search(r"case \"\$kind\" in\s*([\w|]+\))", self.text)
        self.assertIsNotNone(match)
        for kind in match.group(1).rstrip(")").split("|"):
            self.assertRegex(smoke, rf"smoke_boot\.py {kind}\b")

    def test_the_default_scenario_exists_in_the_bench(self) -> None:
        match = re.search(r"scenario:.*?default:\s*(\w+)", self.text, re.S)
        self.assertIsNotNone(match)
        self.assertTrue((ROOT / "Front" / "replays" / f"{match.group(1)}.log").is_file())


if __name__ == "__main__":
    unittest.main()
