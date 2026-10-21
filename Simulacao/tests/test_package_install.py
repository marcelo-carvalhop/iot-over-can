"""Confere instalacao cumulativa em checkout sintetico e reexecucao segura."""
from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]


@unittest.skipUnless((ROOT/"install.sh").is_file(), "Instalador completo existe apenas no pacote de distribuicao")
class InstallerTest(unittest.TestCase):
    def test_cumulative_install_idempotent(self):
        with tempfile.TemporaryDirectory() as t:
            repo=Path(t)
            host=repo/"Codigo/node-can/test/host"
            host.mkdir(parents=True)
            (repo/"Codigo/scripts").mkdir(parents=True)
            (repo/"Codigo/scripts/build_network_sim.sh").write_text("#!/bin/bash\n")
            front=repo/"Front/pico_tui"
            front.mkdir(parents=True)
            (front/"__main__.py").write_text('''import argparse
from pico_tui.app import PicoTuiApp

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--port",default=None)
    parser.add_argument("--mode",default="auto")
    parser.add_argument("--demo",action="store_true")
    parser.add_argument("--replay",default=None)
    parser.add_argument("--security-mode",choices=("off", "presence", "otp"),
        default="presence",
        help="Proteção operacional: off, presence ou otp")
    args = parser.parse_args()
    PicoTuiApp(
        port=args.port,
        mode=args.mode,
        security_mode=args.security_mode,
    ).run()
''')
            # Ancoras reais da etapa 1 e dos caminhos de hardware do simulador.
            (host/"sim_network.cpp").write_text('''#include <vector>
::SimInstance* sim_make_n3b();
namespace {
bool g_verbose = false;
int g_failures = 0;
struct Node {
  uint32_t restarts = 0;
};
Node& probe() { return node(0); }
struct Sensor {
  bool linkUp() {
    Node& n = node(parent);
    if (!n.powered || !n.inst) return false;
    if (!n.inst->apInfo(ssid, psk, channel)) return false;
  }
  void send() {
    node(parent).inst->udpDeliver(d);
  }
  void step() {
        Node& n = node(entry.first);
        if (n.powered && n.inst) n.inst->bleDeliver(mfg, entry.second);
        Node& n = node(entry.first);
        if (!n.powered || !n.inst) continue;
        std::string mfg;
        if (!n.inst->bleOffer(mfg)) continue;
  }
};
void collectSerial() {
    n.log.emplace_back(nowMs(), std::move(line));
}
void routeWifi() {
        if (s.poweredOn && s.parent == n.id && s.port == d.port && s.linkUp()) s.onDatagram(d);
}
void tick() {
    if (!n.powered || !n.inst) continue;
    try {
      n.inst->step();
  routeWifi();
  pumpBus(g_now_us + TICK_US);
}
}  // namespace

uint64_t sim_now_us() { return 0; }
int main(int argc, char** argv) {
  const char* name = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--verbose") == 0) g_verbose = true;
    else if (strcmp(argv[i], "--list") == 0) {
      return 0;
    } else name = argv[i];
  }
  if (!name) {
    fprintf(stderr, "Uso: %s <cenario> [--verbose] | --list\\n", argv[0]);
    return 2;
  }
  if (!ioc_link_hex_to_key(IOT_LINK_MASTER_KEY_HEX, g_master)) return 2;
  ioc_link_derive_network_key(g_master, g_network_key);

  for (const Scenario& s : SCENARIOS) {
    s.run();
  }
}
''')
            for _ in range(2):
                run=subprocess.run(["bash",str(ROOT/"install.sh"),str(repo)], capture_output=True,text=True)
                self.assertEqual(run.returncode,0,run.stdout+'\n'+run.stderr)
            code=(host/"sim_network.cpp").read_text()
            cli=(front/"__main__.py").read_text()
            self.assertEqual(code.count('#include "sim_interactive.inc"'),1)
            self.assertEqual(code.count('#include "sim_factory_decls.inc"'),1)
            self.assertEqual(code.count("simPrepareFactories();"),1)
            self.assertEqual(code.count("simRadioDrop(uint64_t"),1)
            self.assertEqual(cli.count('parser.add_argument("--sim"'),1)
            compile(cli,str(front/"__main__.py"),"exec")
            self.assertTrue((repo/"Simulacao/gen_sim_factories.py").is_file())
            self.assertTrue((repo/"Simulacao/sim_tui_runtime.py").is_file())
