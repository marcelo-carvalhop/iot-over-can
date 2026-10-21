"""Regressões do fluxo criação -> alcance BLE -> descoberta -> associação.

Os testes de comandos não dependem de hardware nem do Node real; a descoberta
pelo firmware permanece coberta pelo ensaio nativo da bancada.
"""
from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from studio.server import StudioState
from studio.protocol import parse_status

UUID='0xE6616408432B6F39'
STATUS = '''#CTRL OK STATUS t_ms=12000 running=1 leader=3 nodes=4 sensors=0 frames=100 bits=10000
#CTRL NODE id=0 powered=1 state=1 tx_disconnected=0 restarts=0 boots_remaining=0 budget_pct=100
#CTRL NODE id=1 powered=1 state=6 tx_disconnected=0 restarts=0 boots_remaining=1 budget_pct=100
#CTRL NODE id=2 powered=1 state=6 tx_disconnected=0 restarts=0 boots_remaining=1 budget_pct=100
#CTRL NODE id=3 powered=1 state=5 tx_disconnected=0 restarts=0 boots_remaining=1 budget_pct=100
#CTRL END
'''


class WirelessFlowTest(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base=Path(self.temporary.name)
        self.studio=StudioState(Path('/tmp/mock_sim'),'/tmp/mock.sock',None,self.base/'results')
        self.base_payload={'type':'sensor.create','uuid':UUID,'profile':'VIBRATION','node':1,'rssi':'-55'}

    def test_create_link_uses_existing_node_and_two_commands(self):
        commands=[]
        def fake_request(socket,command,**kwargs):
            commands.append(command)
            return STATUS if command=='STATUS' else '#CTRL OK '+command+'\n'
        with patch('studio.server.request',side_effect=fake_request):
            response=self.studio.action(self.base_payload)
        self.assertIn('CREATED_AND_LINKED',response)
        self.assertEqual(commands,['STATUS',f'SENSOR ADD {UUID} VIBRATION',f'SENSOR RSSI {UUID} 1 -55'])

    def test_invalid_node_does_not_add_a_sensor(self):
        with patch('studio.server.request',return_value=STATUS) as req:
            with self.assertRaisesRegex(ValueError,'ausente ou desligado'):
                self.studio.action({**self.base_payload,'node':5})
        self.assertEqual(req.call_count,1)

    def test_missing_rssi_rejected_before_any_mutation(self):
        with patch('studio.server.request') as req:
            with self.assertRaises(ValueError):
                self.studio.action({**self.base_payload,'rssi':'-500'})
        req.assert_not_called()

    def test_link_failure_rolls_back_sensor(self):
        commands=[]
        def fake_request(socket,command,**kwargs):
            commands.append(command)
            if command=='STATUS':return STATUS
            if command.startswith('SENSOR RSSI'):raise RuntimeError('Node unreachable')
            return '#CTRL OK '+command+'\n'
        with patch('studio.server.request',side_effect=fake_request):
            with self.assertRaisesRegex(RuntimeError,'inclusão desfeita'):
                self.studio.action(self.base_payload)
        self.assertEqual(commands[-1],f'SENSOR REMOVE {UUID}')

    def test_isolated_sensor_remains_explicit(self):
        commands=[]
        def fake_request(socket,command,**kwargs):
            commands.append(command)
            return '#CTRL OK '+command+'\n'
        with patch('studio.server.request',side_effect=fake_request):
            response=self.studio.action({**self.base_payload,'node':None})
        self.assertEqual(commands,[f'SENSOR ADD {UUID} VIBRATION'])
        self.assertIn('SEM ENLACE',response)

    def test_wireless_discovery_event_visible_in_studio(self):
        trace=self.base/'probe.log'
        trace.write_text(f'@15000 [GW] WIRELESS_CANDIDATE reporter=1 uuid={UUID} rssi=-55\n')
        self.studio.record=trace
        self.studio._tail_log()
        self.assertEqual(len(self.studio.events),1)
        self.assertIn('WIRELESS_CANDIDATE',self.studio.events[0]['message'])

    def test_ui_contains_explicit_link_and_ack_language(self):
        studio=Path(__file__).resolve().parents[1]/'studio'
        html=(studio/'static/index.html').read_text(encoding='utf-8')
        js=(studio/'static/app.js').read_text(encoding='utf-8')
        self.assertIn('new-sensor-node',html)
        self.assertIn('new-sensor-rssi',html)
        self.assertIn("type:'sensor.create'",js)
        self.assertIn('Aguarde confirmação do firmware',js)


if __name__=='__main__':
    unittest.main()
