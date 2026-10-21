"""Compila o include real de controle contra implementacoes pequenas de hardware.

Nao substitui a suite do firmware: detecta regressao de C++ no contrato de
instancias, geradores e comandos antes de chegar ao checkout do usuario.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Simulacao"))
from gen_sim_factories import generate


@unittest.skipUnless(shutil.which("c++"), "Compilador C++ nao disponivel")
class NativeControlTest(unittest.TestCase):
    def test_compile_and_run_dynamic_node_commands(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            instances = generate(5, 3, out)
            factories = "\n".join(f"::SimInstance* sim_make_{name}() {{ return new ::SimInstance(); }}"
                                   for name, _ in instances)
            source = r'''#include <algorithm>
#include <cmath>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <map>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>
struct SimInstance { virtual ~SimInstance() {} virtual int nodeState() const { return 6; } };
''' + factories + r'''
::SimInstance* sim_make_n3b();
#include "sim_factory_decls.inc"
namespace {
uint64_t g_now_us=1000000, g_bus_bits=0;
uint32_t g_bus_frames=0, g_rng=0x1234567u;
unsigned nowMs() { return static_cast<unsigned>(g_now_us/1000); }
struct Node {
    uint8_t id=0;
    std::vector<std::function<::SimInstance*()>> generations;
    size_t nextGeneration=0;
    ::SimInstance* inst=nullptr;
    bool powered=false,txDisconnected=false,bootScheduled=false;
    unsigned restarts=0;
    uint8_t cpuBudgetPct=100;
};
std::vector<Node> g_nodes;
Node& node(uint8_t id) {
    for(auto& n:g_nodes) if(n.id==id) return n;
    std::abort();
}
Node& probe() { return node(0); }
void powerOn(Node& n) {
    if(n.nextGeneration>=n.generations.size()) std::abort();
    n.inst=n.generations[n.nextGeneration++](); n.powered=true;
}
void powerOff(Node& n,bool reboot) {
    delete n.inst; n.inst=nullptr; n.powered=false;
    if(reboot) {n.bootScheduled=true; n.restarts++;}
}
uint8_t currentLeader() {return 3;}
void tick() {g_now_us+=500;}
void probeCommand(const std::string&) {puts("@0 [GW] OK");}
constexpr uint8_t PROFILE_ACCELEROMETER=1, PROFILE_ACOUSTIC_MEMS=2;
struct Sensor {
    enum class Mode {Idle, Discovery, Bound};
    uint64_t uuid=0;
    uint8_t profile=PROFILE_ACCELEROMETER;
    uint8_t ip[4]={192,168,4,2};
    uint16_t port=0;
    uint8_t devKey[32]={0};
    std::map<uint8_t,int> rssi;
    bool poweredOn=true, machineFault=false;
    uint32_t telemetryPeriodMs=500;
    // IOC_USB_HYBRID_STAGE5
    bool usbSource=false;
    uint32_t usbLastRxMs=0, usbSampleVersion=0, usbForwardedVersion=0;
    std::map<std::string,double> usbValues;
    // IOC_USB_FFT_STAGE6: estado minimo usado pelo controle nativo.
    uint32_t usbFftRequest=0,usbFftInjected=0;
    std::vector<float> usbSpectrum;
    float usbSpectrumRateHz=0;
    uint16_t usbSpectrumFftSize=0;
    uint8_t parent=0, child=0;
    Mode mode=Mode::Idle;
};
std::vector<Sensor> g_sensors;
std::map<std::pair<uint64_t,uint8_t>,uint8_t> g_radio_loss_pct;
Sensor& addSensor(uint64_t uuid,uint16_t port,uint8_t ip) {
    Sensor s; s.uuid=uuid; s.port=port; s.ip[3]=ip;
    g_sensors.push_back(s); return g_sensors.back();
}
int g_failures=0;
#include "sim_interactive.inc"
}
int main() {
    g_nodes.push_back(Node{});
    for(int i=1;i<=3;i++) {
        Node n; n.id=static_cast<uint8_t>(i);
        n.generations={sim_make_n1a};
        n.nextGeneration=1; n.powered=true;
        n.inst=new SimInstance();
        g_nodes.push_back(n);
    }
    simPrepareFactories();
    setvbuf(stdout, nullptr, _IOLBF, 0);
    return simInteractiveLoop();
}
'''
            (out / "check.cpp").write_text(source)
            program = out / "check"
            compiled = subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(out),
                "-I", str(ROOT / "Codigo/node-can/test/host"), str(out / "check.cpp"),
                "-o", str(program)
            ], capture_output=True, text=True, timeout=35)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            commands = "PAUSE\nNODE 4 ADD\nSENSOR ADD 0x123456 VIBRATION\nSENSOR RSSI 0x123456 4 -60\nNODE 4 BUDGET 30\nSENSOR LINK 0x123456 4 LOSS 75\nNODE 4 REMOVE\nNODE 4 ADD\nSENSOR SOURCE 0x123456 USB\nSENSOR SAMPLE 0x123456 RMS=0.19 MODE=1\nSTATUS\nQUIT\n"
            run = subprocess.run([str(program)], input=commands, capture_output=True, text=True, timeout=7)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("#CTRL OK NODE id=4 kind=REMOVE", run.stdout)
            self.assertEqual(run.stdout.count("#CTRL OK NODE id=4 kind=ADD"), 2)
            self.assertIn("node=4 rssi=-60", run.stdout)
            self.assertIn("loss_pct=75", run.stdout)
            self.assertIn("#CTRL OK SENSOR SOURCE", run.stdout)
            self.assertIn("#CTRL OK SENSOR SAMPLE", run.stdout)
            self.assertIn("source=USB", run.stdout)
            self.assertIn("kind=BUDGET pct=30", run.stdout)
            self.assertNotIn("#CTRL ERROR", run.stdout)
