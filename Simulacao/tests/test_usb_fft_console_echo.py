"""Regressão: o eco do console do Pico não pode ser lido como resultado de FFT.

O console do sensor acústico (Codigo/node-audio/audio_console.c) ecoa cada
caractere digitado e responde a ``FFT ONCE`` assim:

    EDGE> FFT ONCE\\r\\n          <- eco do comando que a ponte digitou
    OK FFT_ONCE\\r\\n             <- confirmação imediata
    EDGE>                       <- prompt
    ... (até ~1 s: próximo relatório do DSP no núcleo 1)
    \\r\\nFFT VALID=YES BINS=...   <- o resultado de verdade

Antes da correção, a ponte tratava a linha de eco ``FFT ONCE`` como um
``FFT VALID=NO``, repetia o pedido (gerando novo eco) e desistia em
milissegundos, antes de o resultado real chegar. Estes testes emulam o
console byte a byte, inclusive o eco e o prompt.
"""
from __future__ import annotations

import io
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import usb_sensor_bridge as bridge

TEL = (b'TEL PROFILE=ACOUSTIC MODE=MACHINE ACQ=I2S WIN=1024 RATE_EFF_HZ=16000.00 '
       b'INTERVAL_MS=1000 COVERAGE_PCT=100 CPU_PCT=55 CHANNEL=LEFT SPECTRUM_VALID=YES '
       b'OVERLOAD=NO MIC_FAULT=NO LAEQ_DB=61.20 LZEQ_DB=70.10 OCT_DB=40.0,41.0 '
       b'BATT_PCT=100 BATT_MV=5000 DTC=0x0000 DTC_COUNT=0')
FFT_YES = b'FFT VALID=YES BINS=4 RATE_HZ=16000.00 FFT_SIZE=8 UNIT=PA VALUES=0.5,0.25,0.125,0.0625'


class FakeAcousticConsole:
    """Emula o console USB CDC do sensor acústico (eco, OK, prompt, relatórios).

    ``reads_per_report`` leituras vazias equivalem a um relatório do DSP; cada
    relatório imprime TEL e, se houver ``FFT ONCE`` pendente, a linha FFT.
    """

    def __init__(self, fft_line: bytes = FFT_YES, reads_per_report: int = 6,
                 max_reads: int = 400, invalid_reports: int = 0, stop_after_fft: bool = True):
        self.out = bytearray(b'EDGE> ')
        self.writes: list[bytes] = []
        self.fft_line = fft_line
        self.reads_per_report = reads_per_report
        self.max_reads = max_reads
        self.invalid_reports = invalid_reports
        self.stop_after_fft = stop_after_fft
        self.fft_once = False
        self.idle_reads = 0
        self.reads = 0
        self.fft_lines_printed = 0
        self.stop_requested = False

    # -- lado da ponte -> Pico
    def write(self, data: bytes) -> None:
        self.writes.append(data)
        for raw in data.split(b'\n'):
            command = raw.strip()
            if not command:
                continue
            self.out += command + b'\r\n'          # eco do que foi digitado
            if command.upper() == b'FFT ONCE':
                self.fft_once = True
                self.out += b'OK FFT_ONCE\r\n'
            elif command.upper().startswith(b'AUTH UNLOCK'):
                self.out += b'OK AUTH_UNLOCKED LEASE_MS=300000\r\n'
            self.out += b'EDGE> '

    def flush(self) -> None:
        pass

    # -- lado Pico -> ponte
    def _report(self) -> None:
        self.out += b'\r\n' + TEL + b'\r\n'
        if self.fft_once:
            self.fft_once = False
            if self.invalid_reports > 0:
                self.invalid_reports -= 1
                self.out += b'\r\nFFT VALID=NO BINS=0\r\n'
            else:
                self.out += b'\r\n' + self.fft_line + b'\r\n'
            self.fft_lines_printed += 1
            self.out += b'EDGE> '

    def readline(self) -> bytes:
        self.reads += 1
        if self.stop_requested or self.reads > self.max_reads:
            raise KeyboardInterrupt()
        if b'\n' not in self.out:
            self.idle_reads += 1
            if self.idle_reads >= self.reads_per_report:
                self.idle_reads = 0
                self._report()
            return b''
        end = self.out.index(b'\n') + 1
        chunk = bytes(self.out[:end])
        del self.out[:end]
        return chunk


def run_bridge(console: FakeAcousticConsole, deliver_side_effect=None):
    """Executa stream() com um único pedido FFT da Probe pendente."""
    delivered = []

    def fake_deliver(socket_path, uuid, result):
        if deliver_side_effect is not None:
            deliver_side_effect(result)
        delivered.append(result)
        if console.stop_after_fft:
            console.stop_requested = True   # encerra na próxima leitura

    status = {'AUTH': 'UNLOCKED', 'TELEMETRY': 'ON', 'PERIOD_MS': '1000'}
    out, err = io.StringIO(), io.StringIO()
    with (patch.object(bridge, 'fft_request_count', return_value=1),
          patch.object(bridge, 'deliver_fft', side_effect=fake_deliver),
          patch.object(bridge, 'request', return_value='#CTRL OK SENSOR SAMPLE'),
          redirect_stdout(out), redirect_stderr(err)):
        try:
            bridge.stream(console, '/tmp/not-real.sock', '0xE6616408430D5239', 'ACOUSTIC',
                          status, None, passive=True, period_ms=1000, fft_enabled=True)
        except KeyboardInterrupt:
            pass
    return delivered, out.getvalue(), err.getvalue()


class ConsoleEchoTests(unittest.TestCase):
    def test_echo_line_is_not_an_fft_result(self):
        echo = bridge.clean_line(b'EDGE> FFT ONCE\r\n')
        self.assertEqual(echo, 'FFT ONCE')
        self.assertIsNone(bridge.fft_result_state(echo))
        self.assertIsNone(bridge.parse_fft(echo))
        self.assertIsNone(bridge.fft_result_state('OK FFT_ONCE'))
        self.assertIsNone(bridge.fft_result_state('FFT VALIDATION=YES'))
        self.assertEqual(bridge.fft_result_state('FFT VALID=NO BINS=0'), 'NO')
        self.assertEqual(bridge.fft_result_state(FFT_YES.decode()), 'YES')

    def test_echo_does_not_consume_attempts_and_real_fft_is_delivered(self):
        console = FakeAcousticConsole()
        delivered, out, err = run_bridge(console)
        self.assertEqual(len(delivered), 1, out + err)
        self.assertEqual(delivered[0], (16000.0, 8, [0.5, 0.25, 0.125, 0.0625]))
        # Um único pedido ao Pico: o eco não dispara novas tentativas.
        self.assertEqual(sum(w.count(b'FFT ONCE') for w in console.writes), 1)
        self.assertNotIn('nova tentativa', err)
        self.assertNotIn('indisponivel', err)
        self.assertIn('FFT fisica entregue: 4 bins, 16000 Hz', out)

    def test_genuine_valid_no_waits_for_next_report(self):
        console = FakeAcousticConsole(invalid_reports=1)
        delivered, out, err = run_bridge(console)
        self.assertEqual(len(delivered), 1, out + err)
        self.assertEqual(sum(w.count(b'FFT ONCE') for w in console.writes), 2)
        self.assertIn('nova tentativa 2/3', err)

    def test_corrupted_fft_line_does_not_stop_the_bridge(self):
        console = FakeAcousticConsole(
            fft_line=b'FFT VALID=YES BINS=4 RATE_HZ=16000.00 FFT_SIZE=8 UNIT=PA VALUES=0.5,0.25',
            stop_after_fft=False, max_reads=120)
        delivered, out, err = run_bridge(console)
        self.assertEqual(delivered, [])
        self.assertIn('FFT do Pico rejeitada', err)
        # Sem exceção: a ponte continua viva e entregando telemetria.
        self.assertIn('amostras aceitas=', out)

    def test_simulator_rejection_is_reported_without_crashing(self):
        console = FakeAcousticConsole(stop_after_fft=False, max_reads=120)

        def refuse(_result):
            raise RuntimeError('#CTRL ERROR espectro nao solicitado pela Probe')

        delivered, out, err = run_bridge(console, deliver_side_effect=refuse)
        self.assertEqual(delivered, [])
        self.assertIn('simulador recusou o espectro', err)
        self.assertIn('amostras aceitas=', out)


if __name__ == '__main__':
    unittest.main()
