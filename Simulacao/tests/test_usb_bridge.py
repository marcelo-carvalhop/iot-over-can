"""Regressões: parsing USB CDC, identidade e segurança dos dados enviados ao CAN."""
from __future__ import annotations
import sys
import unittest
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from usb_sensor_bridge import convert_tel, fields, clean_line, read_identity, read_admin_token, stream
from unittest.mock import patch

class FormatTest(unittest.TestCase):
    def test_vibration(self):
        sample=convert_tel('TEL MODE=ROTATING ACQ=POLLING WIN=512 FFT_VALID=YES RMS=0.125 '
            'KURT=-0.2 CREST=2.8 PEAK_HZ=31.25 PEAK_AMP=0.02 ENT=0.45 '
            'PPV_MM_S=0.4 BATT_PCT=93 DTC=0x0000','VIBRATION',1000.)
        self.assertIn('RMS=0.125',sample)
        self.assertIn('MODE=1',sample)
        self.assertIn('RATE_HZ=1000',sample)
        self.assertIn('FFT_VALID=1',sample)
    def test_acoustic(self):
        sample=convert_tel('TEL PROFILE=ACOUSTIC MODE=LEVEL ACQ=I2S WIN=1024 RATE_EFF_HZ=16000 '
            'LAEQ_DB=85.5 LCEQ_DB=91.2 CPU_PCT=54 OCT_DB=35.0,-,64.4,70.2 '
            'DTC=0x0020','ACOUSTIC')
        self.assertIn('LAEQ_DB=85.5',sample)
        self.assertIn('OCT_0=35',sample)
        self.assertIn('OCT_2=64.4',sample)
        self.assertIn('DTC=32',sample)
        self.assertNotIn('OCT_1=',sample)
    def test_reject_invalid_and_no_fabricated_sample(self):
        self.assertIsNone(convert_tel('TEL RMS=nan','VIBRATION'))
        self.assertIsNone(convert_tel('TEL PROFILE=ACOUSTIC MODE=LEVEL','ACOUSTIC'))
        self.assertIsNone(convert_tel('EDGE> STATUS MODE=LEVEL','ACOUSTIC'))
    def test_echo_prompt(self):
        self.assertEqual(clean_line(b'EDGE> VERSION\r\n'),'VERSION')

class MockSerial:
    def __init__(self):self.sent=[];self.pending=[]
    def write(self,data):
        self.sent.append(data)
        self.pending.extend([b'EDGE> VERSION\r\n',
            b'VERSION PROTOCOL=6 NODE_UUID=0xE6616408430D5239 PROFILE=ACOUSTIC\r\n',
            b'STATUS PROFILE=ACOUSTIC NET=DISABLED AUTH=LOCKED TELEMETRY=OFF\r\n'])
    def flush(self):pass
    def readline(self):return self.pending.pop(0) if self.pending else b''

class IdentityTest(unittest.TestCase):
    def test_identity_physical(self):
        uuid,profile,status=read_identity(MockSerial(),timeout=.5)
        self.assertEqual(uuid,'0xE6616408430D5239')
        self.assertEqual(profile,'ACOUSTIC')
        self.assertEqual(status['NET'],'DISABLED')
    def test_auth_token_permission(self):
        import tempfile,os
        with tempfile.TemporaryDirectory() as t:
            p=Path(t)/'token'
            p.write_text('0x12345678')
            os.chmod(p,0o644)
            with self.assertRaises(ValueError):read_admin_token(p)
            os.chmod(p,0o600)
            self.assertEqual(read_admin_token(p),'0x12345678')

class DataPathTest(unittest.TestCase):
    def test_wired_measurement_sent_to_simulator(self):
        class FakeSource:
            def __init__(self):self.lines=[b'TEL MODE=ROTATING ACQ=POLLING RMS=0.28 WIN=512\r\n']
            def readline(self):
                if self.lines:return self.lines.pop(0)
                raise KeyboardInterrupt
        calls=[]
        def capture(sock,cmd,timeout=6):
            calls.append((sock,cmd))
            return '#CTRL OK SENSOR SAMPLE'
        with patch('usb_sensor_bridge.request',side_effect=capture):
            with self.assertRaises(KeyboardInterrupt):
                # O teste valida somente o caminho de dados. No modo passivo,
                # a ponte nao tenta configurar um sensor fisico nem exige token.
                stream(FakeSource(),'/tmp/control.sock','0x0000000000000123','VIBRATION',
                       {'TELEMETRY':'ON','RATE_HZ':'1000'},None,passive=True)
        self.assertEqual(len(calls),1)
        self.assertIn('SENSOR SAMPLE 0x0000000000000123',calls[0][1])
        self.assertIn('RMS=0.28',calls[0][1])

    def test_locked_sensor_without_stream_does_not_bypass_auth(self):
        class FakeSource:
            def write(self, data):
                raise AssertionError('Nao deve enviar comando a dispositivo bloqueado')
            def readline(self):
                raise AssertionError('Nao deve aguardar leitura')
        with self.assertRaisesRegex(RuntimeError, 'AUTH=LOCKED'):
            stream(FakeSource(), '/tmp/control.sock', '0x0000000000000123',
                   'VIBRATION', {'AUTH':'LOCKED','TELEMETRY':'OFF'}, None)

if __name__=='__main__':unittest.main()
