"""Testes autônomos para a etapa 4. Sem hardware, sem acesso externo à rede."""
from __future__ import annotations

import json
import os
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
from http.client import HTTPConnection
from pathlib import Path

SIM=Path(__file__).resolve().parents[1]
ROOT=SIM.parent
sys.path.insert(0,str(SIM))
from studio.protocol import build_command,parse_status,validate_native_command
from studio.experiments import validate_spec,run_experiment
from studio.server import Handler,LocalServer,StudioState

FAKE_SIM=r'''#!/usr/bin/env python3
import sys
running='--interactive-paused' not in sys.argv
print('#CTRL OK READY mode=INTERACTIVE protocol=1',flush=True)
t_ms=14300
frames=22
bits=2200
while True:
    line=sys.stdin.readline()
    if not line:break
    words=line.strip().split()
    if not words:continue
    if words[0]=='STATUS':
        print(f'#CTRL OK STATUS t_ms={t_ms} running={int(running)} leader=3 nodes=4 sensors=0 frames={frames} bits={bits}')
        print('#CTRL NODE id=0 powered=1 state=1 tx_disconnected=0 restarts=0 boots_remaining=0 budget_pct=100')
        print('#CTRL NODE id=1 powered=1 state=6 tx_disconnected=0 restarts=0 boots_remaining=1 budget_pct=100')
        print('#CTRL NODE id=2 powered=1 state=6 tx_disconnected=0 restarts=0 boots_remaining=1 budget_pct=100')
        print('#CTRL NODE id=3 powered=1 state=5 tx_disconnected=0 restarts=0 boots_remaining=1 budget_pct=100')
        print('#CTRL END',flush=True)
    elif words[0]=='STEP':
        ms=int(words[1]);t_ms+=ms;frames+=ms//100;bits+=(ms//100)*100
        print(f'@{t_ms} [GW] BUS_STATS t_ms={t_ms} win_ms=1000 frames=16 bits=1700 load=0.34 peak=0.41 rx_err=0 tx_err=0',flush=True)
        print(f'#CTRL OK STEP t_ms={t_ms}',flush=True)
    elif words[0]=='SEED':
        print(f'#CTRL OK SEED value={int(words[1])}',flush=True)
    elif words[0]=='PAUSE':
        running=False;print(f'#CTRL OK PAUSE t_ms={t_ms}',flush=True)
    elif words[0]=='RESUME':
        running=True;print(f'#CTRL OK RESUME t_ms={t_ms}',flush=True)
    elif words[0]=='QUIT':
        print('#CTRL OK QUIT',flush=True);break
    elif words[0]=='PROBE':
        print('#CTRL OK PROBE',flush=True)
    else:
        print('#CTRL OK '+words[0],flush=True)
'''

STATUS='''#CTRL OK STATUS t_ms=20500 running=1 leader=3 nodes=2 sensors=1 frames=100 bits=9000
#CTRL NODE id=0 powered=1 state=1 tx_disconnected=0 restarts=0 boots_remaining=0 budget_pct=100
#CTRL NODE id=3 powered=1 state=5 tx_disconnected=0 restarts=0 boots_remaining=1 budget_pct=80
#CTRL SENSOR uuid=0xE6616408432B6F39 profile=ACOUSTIC powered=1 parent=3 child=1 mode=2 links=1
#CTRL LINK uuid=0xE6616408432B6F39 node=3 rssi=-50 loss_pct=10
#CTRL END
'''

class ProtocolTests(unittest.TestCase):
    def test_status_with_links(self):
        parsed=parse_status(STATUS)
        self.assertEqual(parsed['leader'],3)
        self.assertEqual(parsed['nodes'][1]['budget_pct'],80)
        self.assertEqual(parsed['links'][0]['loss_pct'],10)
        self.assertEqual(parsed['sensors'][0]['uuid'],'0xE6616408432B6F39')

    def test_safe_command_builder(self):
        self.assertEqual(build_command({'type':'sensor.rssi','uuid':'0xe6616408432b6f39','node':2,'rssi':-65}),
            'SENSOR RSSI 0xE6616408432B6F39 2 -65')
        self.assertEqual(build_command({'type':'node.budget','id':4,'pct':75}),'NODE 4 BUDGET 75')
        self.assertEqual(build_command({'type':'probe.bind','uuid':'0x10','node':3,'policy':'AUTO'}),
                         'PROBE WIRELESS BIND 3 0x0000000000000010 AUTO')
        with self.assertRaises(ValueError):build_command({'type':'node.remove','id':'1\nQUIT'})
        with self.assertRaises(ValueError):build_command({'type':'sensor.loss','uuid':'0x10','node':2,'pct':101})
        with self.assertRaises(ValueError):build_command({'type':'probe.raw','command':'rm -rf /'})
        with self.assertRaises(ValueError):validate_native_command('PROBE QUIT')

    def test_experiment_validation(self):
        self.assertEqual(validate_spec({'name':'base','duration_ms':12000,'steps':[]})['repetitions'],1)
        with self.assertRaises(ValueError):validate_spec({'name':'x','duration_ms':1000,'steps':[{'at_ms':3,'command':'QUIT'}]})
        with self.assertRaises(ValueError):validate_spec({'name':'x','duration_ms':1000,'steps':[{'at_ms':300,'command':'NODE 2 TX OFF'},{'at_ms':200,'command':'NODE 2 TX ON'}]})
        with self.assertRaises(ValueError):validate_spec({'name':'x','duration_ms':1000,'steps':[{'at_ms':0,'command':'NODE 2 TX OFF\nQUIT'}]})

class ExperimentTests(unittest.TestCase):
    def test_isolated_repetitions_and_artifacts(self):
        with tempfile.TemporaryDirectory() as temp:
            base=Path(temp);binary=base/'fake_sim.py';binary.write_text(FAKE_SIM);binary.chmod(0o755)
            spec={'name':'Ensaio', 'duration_ms':130000,'repetitions':2,
                  'steps':[{'at_ms':1000,'command':'NODE 2 BUDGET 75'}]}
            result=run_experiment(binary,spec,base/'results')
            self.assertEqual(result['status'],'completed')
            self.assertEqual(len(result['repetitions']),2)
            self.assertEqual([x['frames_delta'] for x in result['repetitions']],[1300,1300])
            self.assertEqual([x['final_t_ms']-x['initial_t_ms'] for x in result['repetitions']],[130000,130000])
            path=base/'results'/result['id']
            self.assertTrue((path/'summary.json').is_file())
            self.assertTrue((path/'comparison.csv').is_file())
            self.assertEqual(result['repetitions'][0]['seed'],result['repetitions'][1]['seed'])
            self.assertIn('BUS_STATS', (path/'run-01/trace.log').read_text())
            self.assertIn('load_pct_probe',(path/'run-02/bus_stats.csv').read_text())
            studio=StudioState(binary,None,None,base/'results')
            self.assertGreater(len(studio.export(result['id'])),200)
            with self.assertRaises(ValueError):studio.export('../forbidden')

class ServerTests(unittest.TestCase):
    def test_tail_and_loopback_authorization(self):
        with tempfile.TemporaryDirectory() as temp:
            base=Path(temp);record=base/'probe.log'
            record.write_text('@1234 [GW] BUS_STATS t_ms=1234 win_ms=1000 frames=1 bits=100 load=0.02 peak=0.05\n'
                              '@1250 [GW] WIRELESS_ASSOC node=2 child=1 state=ONLINE\n')
            state=StudioState(Path('/tmp/unused'),'/tmp/unused',record,base/'results')
            state._tail_log()
            self.assertEqual(state.latest_probe['load_pct'],.02)
            self.assertEqual(len(state.events),1)
            state._tail_log()
            self.assertEqual(len(state.events),1)
            server=LocalServer(('127.0.0.1',0),state)
            thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
            try:
                conn=HTTPConnection('127.0.0.1',server.server_port,timeout=2)
                conn.request('GET','/api/state');r=conn.getresponse()
                self.assertEqual(r.status,403);r.read();conn.close()
                conn=HTTPConnection('127.0.0.1',server.server_port,timeout=2)
                conn.request('GET','/',headers={});r=conn.getresponse()
                page=r.read().decode()
                self.assertEqual(r.status,200)
                self.assertIn(state.token,page)
                self.assertIn('Simulation Studio',page)
                conn.close()
                conn=HTTPConnection('127.0.0.1',server.server_port,timeout=2)
                body=json.dumps({'type':'node.remove','id':2})
                conn.request('POST','/api/action',body=body,
                    headers={'X-IOC-Token':state.token,'Origin':'http://evil.example',
                             'Content-Type':'application/json'})
                r=conn.getresponse();self.assertEqual(r.status,403);r.read();conn.close()
            finally:
                server.shutdown();server.server_close();thread.join(timeout=2)

class PatchTests(unittest.TestCase):
    def test_cplusplus_patches_are_idempotent(self):
        with tempfile.TemporaryDirectory() as temp:
            base=Path(temp);source=base/'sim_network.cpp';inc=base/'sim_interactive.inc'
            source.write_text('''int main() {
  bool interactive = false;
    else if (strcmp(argv[i], "--interactive") == 0) interactive = true;
    return simInteractiveLoop();
}''')
            inc.write_text('''void simControl() {
  puts("#CTRL HELP STATUS | PAUSE | RESUME | STEP <ms> | QUIT");
    if (action == "PAUSE") {
    }
}
int simInteractiveLoop() {
  bool running = true;
}''')
            patches=[(SIM/'patch_stage4.py',inc),(SIM/'patch_stage4_entry.py',source)]
            for _ in range(2):
                for patch,target in patches:
                    run=subprocess.run([sys.executable,str(patch),str(target)],capture_output=True,text=True)
                    self.assertEqual(run.returncode,0,run.stdout+'\n'+run.stderr)
            self.assertEqual(source.read_text().count('"--interactive-paused"'),1)
            self.assertEqual(inc.read_text().count('bool startPaused = false'),1)

class Stage4InstallerTests(unittest.TestCase):
    def test_install_over_existing_stage23_twice(self):
        installer=ROOT/'install.sh'
        if not installer.is_file():self.skipTest('Instalador disponível apenas no pacote de entrega')
        with tempfile.TemporaryDirectory() as temp:
            repo=Path(temp)
            host=repo/'Codigo/node-can/test/host';host.mkdir(parents=True)
            (repo/'Front/pico_tui').mkdir(parents=True)
            (repo/'Front/pico_tui/__main__.py').write_text('# TUI etapa 3\n')
            (repo/'Simulacao').mkdir()
            (repo/'Simulacao/sim_bridge.py').write_text('# bridge existente\n')
            baseline=ROOT/'stage23/Codigo/node-can/test/host/sim_interactive.inc'
            (host/'sim_interactive.inc').write_bytes(baseline.read_bytes())
            (host/'sim_network.cpp').write_text(\
                '  bool interactive = false;\n' \
                '    else if (strcmp(argv[i], "--interactive") == 0) interactive = true;\n' \
                '    simPrepareFactories();\n' \
                '    return simInteractiveLoop();\n')
            for _ in range(2):
                process=subprocess.run(['bash',str(installer),str(repo)],capture_output=True,text=True)
                self.assertEqual(process.returncode,0,process.stderr)
            self.assertEqual((host/'sim_network.cpp').read_text().count('"--interactive-paused"'),1)
            self.assertEqual((host/'sim_interactive.inc').read_text().count('if (action == "SEED")'),1)
            self.assertTrue((repo/'Simulacao/studio/static/app.js').is_file())
            self.assertEqual((repo/'Simulacao/sim_bridge.py').read_text(),'# bridge existente\n')

if __name__=='__main__':unittest.main()
