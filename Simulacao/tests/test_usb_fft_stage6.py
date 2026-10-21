"""Native USB FFT input validation and injection without exposing user secrets."""
from __future__ import annotations
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

PACKAGE=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PACKAGE))
from usb_sensor_bridge import parse_fft,deliver_fft,fft_request_count

class FftProtocolTests(unittest.TestCase):
    def test_real_pico_format_256_bins(self):
        raw='FFT VALID=YES BINS=256 RATE_HZ=16000.00 FFT_SIZE=512 UNIT=PA VALUES='+','.join(['0.05599']+['0.000123']*255)
        rate,size,values=parse_fft(raw)
        self.assertEqual((rate,size,len(values)),(16000,512,256))
        self.assertAlmostEqual(values[0],.05599)
    def test_reject_missing_and_nonfinite_or_incompatible(self):
        for message in ('FFT VALID=YES BINS=256 RATE_HZ=16000 FFT_SIZE=512 VALUES=0.1',
                        'FFT VALID=YES BINS=2 RATE_HZ=16000 FFT_SIZE=4 VALUES=nan,0.1',
                        'FFT VALID=YES BINS=2 RATE_HZ=16000 FFT_SIZE=4 VALUES=-0.1,0.2',
                        'FFT VALID=YES BINS=2 RATE_HZ=16000 FFT_SIZE=8 VALUES=0.1,0.2'):
            with self.subTest(message=message),self.assertRaises(ValueError):parse_fft(message)
        self.assertIsNone(parse_fft('FFT VALID=NO BINS=0'))
    def test_dispatch_and_request(self):
        commands=[]
        def fake_request(sock,cmd,timeout=15):
            commands.append(cmd)
            return '#CTRL OK SENSOR FFT_QUERY uuid=0xA request=9' if 'FFT_QUERY' in cmd else '#CTRL OK SENSOR SPECTRUM bins=2'
        with patch('usb_sensor_bridge.request',side_effect=fake_request):
            self.assertEqual(fft_request_count('/tmp/mock','0xA'),9)
            deliver_fft('/tmp/mock','0xA',(16000,4,[.01,.02]))
        self.assertIn('FFT_SIZE=4 VALUES=0.01,0.02',commands[1])

if __name__=='__main__':unittest.main()
