"""Firmware serial simulado para os testes ponta a ponta da TUI."""
from __future__ import annotations

import os
import pty
import threading
import time
import tty


class FakeFirmware:
    def __init__(self) -> None:
        self.master_fd, self.slave_fd = pty.openpty()
        tty.setraw(self.slave_fd)
        self.slave_name = os.ttyname(self.slave_fd)
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._linebuf = ""
        self.simulate = False
        self.telemetry = False
        self.acquisition = "POLLING"
        self.mode = "STRUCTURAL"
        self.window = "HANN"
        self.window_size = 512
        self.rate_hz = 1000.0
        self.stalta = 4.0
        self.gain = 1.0
        self.dtc_code = 0
        self.fft_armed = False
        self.received_commands: list[str] = []
        self._staged: dict[str, str] = {}

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        try:
            os.close(self.master_fd)
        except OSError:
            pass

    def _write(self, text: str) -> None:
        os.write(self.master_fd, text.encode("utf-8"))

    def _prompt(self) -> None:
        self._write("EDGE> ")

    def _run(self) -> None:
        time.sleep(0.05)
        self._write("\nEDGE DSP VIBRATION NODE\n")
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
            f"STATUS NET=DISCOVERY MODE={self.mode} ACQ={self.acquisition} WINDOW={self.window} "
            f"WINDOW_SIZE={self.window_size} RATE_HZ={self.rate_hz:.2f} STALTA={self.stalta:.3f} "
            f"GAIN={self.gain:.3f} DTC=0x{self.dtc_code:04X} DTC_COUNT={1 if self.dtc_code else 0} "
            f"MPU=YES SIM={'YES' if self.simulate else 'NO'} TELEMETRY={'ON' if self.telemetry else 'OFF'} "
            "PERIOD_MS=1000 BATT_PCT=255 BATT_MV=65535 DRDY_IRQ=0 DRDY_MISSED=0\n"
        )

    def _telemetry_line(self) -> str:
        fft_valid = self.mode != "SEISMIC"
        return (
            f"TEL MODE={self.mode} ACQ={self.acquisition} WIN={self.window_size} AXIS=VECTOR "
            f"FFT_VALID={'YES' if fft_valid else 'NO'} RMS=0.07136 KURT=2.19474 CREST=3.9580 "
            f"PEAK_HZ={17.578 if fft_valid else 0.0:.3f} PEAK_AMP={0.011725 if fft_valid else 0.0:.6f} "
            f"ENT={0.9221 if fft_valid else 0.0:.4f} PPV_MM_S=1.6673 STA_LTA=NO CLIP=NO "
            f"BATT_PCT=255 BATT_MV=65535 DTC=0x{self.dtc_code:04X} DTC_COUNT={1 if self.dtc_code else 0}\n"
        )

    def _process_line(self, line: str) -> None:
        line = line.strip()
        self.received_commands.append(line)
        tokens = line.split()
        if not tokens:
            return
        cmd = tokens[0].upper()

        if line == "!":
            self.telemetry = False
            self._write("OK TELEMETRY=OFF\n")
        elif cmd in ("HELP", "MENU"):
            self._write("\nEDGE DSP SERIAL CONSOLE - ASCII MODE\nCommands:\n  HELP\n\n")
        elif cmd == "STATUS":
            self._write(self._status_line())
        elif cmd == "GET":
            self._write(
                f"STAGED MODE={self.mode} RATE_HZ={self.rate_hz:.2f} WINDOW={self.window} "
                f"WINDOW_SIZE={self.window_size} STALTA={self.stalta:.3f} GAIN={self.gain:.3f}\n"
            )
        elif cmd == "SET" and len(tokens) >= 3:
            field = tokens[1].upper()
            value = tokens[2].upper()
            self._staged[field] = value
            self._write(f"OK STAGED {field}={value}\n")
        elif cmd == "APPLY":
            self._write("OK APPLY_QUEUED\n")
            self._apply_staged()
            self._write(
                f"CONFIG_APPLIED MODE={self.mode} RATE_REQ={self.rate_hz:.2f} RATE_EFF={self.rate_hz:.2f} "
                f"WINDOW={self.window} WINDOW_REQ={self.window_size} WINDOW_EFF={self.window_size} ACQ=POLLING\n"
            )
        elif cmd in ("TELEMETRY", "TEL") and len(tokens) >= 2:
            action = tokens[1].upper()
            if action == "ON":
                self.telemetry = True
                self._write("OK TELEMETRY=ON PERIOD_MS=1000\n")
            elif action == "OFF":
                self.telemetry = False
                self._write("OK TELEMETRY=OFF\n")
            elif action == "ONCE":
                self._write(self._telemetry_line())
                if self.fft_armed and self.mode != "SEISMIC":
                    self._write("FFT VALID=YES BINS=8 VALUES=0.1,0.2,0.8,1.4,0.7,0.3,0.2,0.1\n")
                    self.fft_armed = False
            elif action in {"FAST", "SLOW"}:
                self._write(f"OK TELEMETRY={action}\n")
            elif action == "PERIOD" and len(tokens) >= 3:
                self._write(f"OK TELEMETRY=PERIOD PERIOD_MS={tokens[2]}\n")
        elif cmd == "FFT" and len(tokens) >= 2 and tokens[1].upper() == "ONCE":
            if self.mode == "SEISMIC":
                self._write("FFT VALID=NO BINS=0 VALUES=\n")
            else:
                self.fft_armed = True
                self._write("OK FFT=ARMED\n")
        elif cmd in ("SIMULATE", "SIM") and len(tokens) >= 2:
            self.simulate = tokens[1].upper() == "ON"
            self.acquisition = "SIM" if self.simulate else "POLLING"
            self._write(f"OK SIMULATE={'ON' if self.simulate else 'OFF'}\n")
        elif cmd == "ACQ" and len(tokens) >= 2 and tokens[1].upper() == "POLLING":
            self.acquisition = "POLLING"
            self._write("OK ACQ=POLLING_RESTARTED\n")
        elif cmd == "ACQ" and len(tokens) >= 2 and tokens[1].upper() == "DRDY":
            self._write("ERR DRDY disabled in polling baseline. Use ACQ POLLING.\n")
        elif cmd == "PING":
            self._write("PONG UPTIME_MS=12345\n")
        elif cmd == "DTC" and len(tokens) >= 2 and tokens[1].upper() == "CLEAR":
            self.dtc_code = 0
            self._write("OK DTC=CLEARED DTC_COUNT=0\n")
        elif cmd == "DTC":
            self._write(f"DTC ACTIVE=0x{self.dtc_code:04X} COUNT={1 if self.dtc_code else 0}\n")
        elif cmd == "NET":
            self._write("NET STATE=DISCOVERY\n")
        elif cmd == "VERSION":
            self._write("VERSION PROTOCOL=4 NODE_UUID=0x10A4\n")
        elif cmd == "RESET":
            self._write("OK RESETTING\n")
        else:
            self._write("ERR Unknown command. Use HELP.\n")

    def _apply_staged(self) -> None:
        if "MODE" in self._staged:
            self.mode = self._staged["MODE"]
        if "RATE" in self._staged:
            self.rate_hz = float(self._staged["RATE"])
        if "WINDOW" in self._staged:
            self.window = self._staged["WINDOW"]
        if "WINDOW_SIZE" in self._staged:
            self.window_size = int(self._staged["WINDOW_SIZE"])
        if "STALTA" in self._staged:
            self.stalta = float(self._staged["STALTA"])
        if "GAIN" in self._staged:
            self.gain = float(self._staged["GAIN"])
        self._staged.clear()

    def inject_dtc_event(self, code: int = 0x2002) -> None:
        self.dtc_code = code
        self._write(f"\r\nDTC_EVENT CODE=0x{code:04X} SYMPTOM=0x16 SEVERITY=1 TS_MS=99999\n")
        self._prompt()

    def inject_net_event(self, state: str = "BOUND") -> None:
        self._write(f"\r\nNET_EVENT STATE={state}\n")
        self._prompt()
