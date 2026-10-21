"""Regressoes das etapas 2/3 sem depender do firmware ESP32."""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Simulacao"))
from gen_sim_factories import generate


class FactoryTest(unittest.TestCase):
    def test_generates_unique_factories_and_limits(self):
        with tempfile.TemporaryDirectory() as t:
            entries = generate(5, 3, Path(t))
            self.assertEqual(len(entries), 16)
            self.assertEqual(len({x[0] for x in entries}), len(entries))
            registry = (Path(t) / "sim_factory_registry.inc").read_text()
            declarations = (Path(t) / "sim_factory_decls.inc").read_text()
            self.assertIn("{5, {sim_make_n5g0, sim_make_n5g1, sim_make_n5g2}}", registry)
            self.assertIn("::SimInstance* sim_make_n1g2();", declarations)
            self.assertNotIn("::SimInstance* sim_make_n1a();", declarations)
        for n, generations in ((2, 2), (32, 2), (6, 1), (6, 17)):
            with tempfile.TemporaryDirectory() as t:
                with self.assertRaises(ValueError):
                    generate(n, generations, Path(t))

    def test_control_layer_has_dynamic_nodes(self):
        text = (ROOT / "Codigo/node-can/test/host/sim_interactive.inc").read_text()
        for snippet in ("kind == \"ADD\"", "kind == \"REMOVE\"", "simGeneratedFactories()", "g_node_consumed[key]"):
            self.assertIn(snippet, text)


class TuiPatchTest(unittest.TestCase):
    def test_tui_patch_idempotent_and_syntax(self):
        with tempfile.TemporaryDirectory() as t:
            source = Path(t) / "__main__.py"
            source.write_text('''import argparse
from pico_tui.app import PicoTuiApp

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=None)
    parser.add_argument("--mode", default="auto")
    parser.add_argument("--demo", action="store_true")
    parser.add_argument("--replay", default=None)
    parser.add_argument(
        "--security-mode", choices=("off", "presence", "otp"),
        default="presence",
        help="Proteção operacional: off, presence ou otp",
    )
    args = parser.parse_args()
    PicoTuiApp(
        port=args.port,
        mode=args.mode,
        demo=args.demo,
        security_mode=args.security_mode,
    ).run()

if __name__ == "__main__":
    main()
''')
            for _ in range(2):
                p = subprocess.run([sys.executable, str(ROOT / "Simulacao/patch_tui.py"), str(source)],
                                   capture_output=True, text=True)
                self.assertEqual(p.returncode, 0, p.stderr)
            text = source.read_text()
            compile(text, str(source), "exec")
            self.assertEqual(text.count('parser.add_argument("--sim"'), 1)
            self.assertIn('default=None,\n        help="Proteção operacional:', text)
            self.assertIn('security_mode=args.security_mode or ("off" if args.sim else "presence")', text)


if __name__ == "__main__":
    unittest.main()
