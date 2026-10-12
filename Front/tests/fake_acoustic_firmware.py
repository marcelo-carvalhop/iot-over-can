"""Console serial simulado do sensor acústico (Codigo/node-audio/audio_console.c).

Reaproveita o pty e o laço de ``FakeFirmware`` e troca as respostas pelas do
firmware INMP441: STATUS/VERSION com PROFILE=ACOUSTIC, telemetria de um
segundo, espectro em Pa, CAL, MEASURE e BASELINE.
"""
from __future__ import annotations

from tests.fake_firmware import FakeFirmware


class FakeAcousticFirmware(FakeFirmware):
    def __init__(self) -> None:
        super().__init__()
        self.mode = "LEVEL"
        self.rate_hz = 16000.0
        self.window_size = 1024
        self.stalta = 8.0
        self.lzeq_db = 93.0          # calibrador de 94 dB lido 1 dB abaixo
        self.measure_resets = 0
        self.baseline = "NONE"

    def _run(self) -> None:
        import os
        import time

        time.sleep(0.05)
        self._write("\nEDGE DSP ACOUSTIC NODE (INMP441, RP2040)\n")
        self._write("ASCII serial console ready. Type HELP and press Enter.\n")
        self._prompt()
        while not self._stop.is_set():
            try:
                chunk = os.read(self.master_fd, 256)
            except OSError:
                return
            if not chunk:
                return
            for byte in chunk:
                if byte == 0x03:
                    self.telemetry = False
                    self.received_commands.append("<CTRL-C>")
                    self._write("\r\nOK TELEMETRY=OFF\n")
                    self._prompt()
                    continue
                c = chr(byte)
                if c in ("\r", "\n"):
                    self._write("\r\n")
                    if self._linebuf:
                        self._process_line(self._linebuf)
                        self._linebuf = ""
                    self._prompt()
                else:
                    self._linebuf += c
                    self._write(c)

    def _status_line(self) -> str:
        return (
            f"STATUS PROFILE=ACOUSTIC NET=DISCOVERY MODE={self.mode} ACQ=I2S RATE_HZ={self.rate_hz:.2f} "
            f"RATE_EFF_HZ={self.rate_hz:.2f} WINDOW={self.window} WINDOW_SIZE={self.window_size} "
            f"THRESHOLD={self.stalta:.3f} STALTA={self.stalta:.3f} GAIN={self.gain:.4f} CHANNEL=LEFT MIC=OK "
            f"CPU_PCT=37 COVERAGE_PCT=100 CHIP=RP2040 BOARD=pico_w DTC=0x{self.dtc_code:04X} "
            f"DTC_COUNT={1 if self.dtc_code else 0} TELEMETRY={'ON' if self.telemetry else 'OFF'} PERIOD_MS=1000 "
            "BATT_PCT=255 BATT_MV=65535 AUTH=LOCKED WIFI_PROVISIONED=NO BLE_BEACON=ON\n"
        )

    def _telemetry_line(self) -> str:
        line = (
            f"TEL PROFILE=ACOUSTIC MODE={self.mode} ACQ=I2S WIN={self.window_size} RATE_EFF_HZ={self.rate_hz:.2f} "
            "INTERVAL_MS=1000 COVERAGE_PCT=100 CPU_PCT=37 CHANNEL=LEFT SPECTRUM_VALID=YES OVERLOAD=NO MIC_FAULT=NO "
            f"LAEQ_DB={self.lzeq_db:.2f} LCEQ_DB={self.lzeq_db:.2f} LZEQ_DB={self.lzeq_db:.2f} "
            f"LZPEAK_DB={self.lzeq_db + 3.01:.2f} LAF_DB={self.lzeq_db:.2f} LAFMAX_DB={self.lzeq_db + 0.1:.2f} "
            f"LAFMIN_DB={self.lzeq_db - 0.1:.2f} CREST=1.41 KURT=-1.50 PEAK_HZ=1000.0 CENTROID_HZ=1000 "
            "FLATNESS=0.0010 OCT_DB=-,-,-,-,40.0,93.0,40.0,-,-,-"
        )
        if self.mode == "LEVEL":
            line += (
                f" LEQ_T_DB={self.lzeq_db:.2f} L10_DB={self.lzeq_db:.2f} L50_DB={self.lzeq_db:.2f} "
                f"L90_DB={self.lzeq_db:.2f} DOSE_NR15_PCT=12.5 DOSE_NHO01_PCT=25.0 ELAPSED_S=1800 DOSE_LIMIT=NO"
            )
        return line + f" BATT_PCT=255 BATT_MV=65535 DTC=0x{self.dtc_code:04X} DTC_COUNT={1 if self.dtc_code else 0}\n"

    def _fft_line(self) -> str:
        values = ",".join(f"{0.002 + (1.0 if i == 32 else 0.0):.6g}" for i in range(64))
        return f"FFT VALID=YES BINS=64 RATE_HZ={self.rate_hz:.2f} FFT_SIZE=128 UNIT=PA VALUES={values}\n"

    def _process_line(self, line: str) -> None:
        tokens = line.strip().split()
        cmd = tokens[0].upper() if tokens else ""
        if cmd == "VERSION":
            self.received_commands.append(line.strip())
            self._write("VERSION PROTOCOL=6 NODE_UUID=0xE6616408432B7A11 PROFILE=ACOUSTIC CHIP=RP2040 BOARD=pico_w\n")
            return
        if cmd == "CAL" and len(tokens) >= 2:
            self.received_commands.append(line.strip())
            reference = float(tokens[1])
            offset = reference - self.lzeq_db
            self.gain *= 10.0 ** (offset / 20.0)
            self.lzeq_db = reference
            self._write(
                f"OK CAL MEASURED_DB={reference - offset:.2f} REFERENCE_DB={reference:.2f} "
                f"OFFSET_DB={offset:+.2f} GAIN={self.gain:.4f}\n"
            )
            self._write(self._applied_line())
            return
        if cmd == "MEASURE":
            self.received_commands.append(line.strip())
            self.measure_resets += 1
            self._write("OK MEASURE_RESET_QUEUED\n\r\nOK MEASURE_RESET\n")
            return
        if cmd == "BASELINE":
            self.received_commands.append(line.strip())
            if len(tokens) >= 3 and tokens[1].upper() == "LEARN" and self.mode == "MACHINE":
                self.baseline = "LEARNING"
                self._write("OK BASELINE_LEARN_QUEUED\n\r\nOK BASELINE_LEARN\n")
            elif len(tokens) >= 2 and tokens[1].upper() == "CLEAR":
                self.baseline = "NONE"
                self._write("OK BASELINE_CLEAR_QUEUED\n\r\nOK BASELINE_CLEAR\n")
            else:
                self._write("OK BASELINE_LEARN_QUEUED\n\r\nERR BASELINE_LEARN_REJECTED (use MODE=MACHINE and 5..3600 s)\n")
            return
        if cmd == "TELEMETRY" and len(tokens) >= 2 and tokens[1].upper() == "ONCE":
            self.received_commands.append(line.strip())
            self._write("OK TELEMETRY=ONCE\n")
            self._write("\r\n" + self._telemetry_line())
            if self.fft_armed:
                self._write("\r\n" + self._fft_line())
                self.fft_armed = False
            return
        if cmd == "APPLY":
            self.received_commands.append(line.strip())
            self._write("OK APPLY_QUEUED\n")
            self._apply_staged()
            self._write(self._applied_line())
            return
        super()._process_line(line)

    def _apply_staged(self) -> None:
        if "THRESHOLD" in self._staged:
            self._staged["STALTA"] = self._staged.pop("THRESHOLD")
        if "FFT" in self._staged:
            self._staged["WINDOW_SIZE"] = self._staged.pop("FFT")
        super()._apply_staged()

    def _applied_line(self) -> str:
        return (
            f"\r\nCONFIG_APPLIED MODE={self.mode} RATE_REQ={self.rate_hz:.2f} RATE_EFF={self.rate_hz:.2f} "
            f"WINDOW={self.window} WINDOW_SIZE={self.window_size} THRESHOLD={self.stalta:.3f} "
            f"STALTA={self.stalta:.3f} GAIN={self.gain:.4f} ACQ={'IDLE' if self.mode == 'IDLE' else 'I2S'}\n"
        )
