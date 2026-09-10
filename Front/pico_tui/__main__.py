from __future__ import annotations

import argparse

from pico_tui.app import PicoTuiApp


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="iot-over-can-tui",
        description="TUI de engenharia para sensor wireless, gateway ESP32 e rede CAN FD.",
    )
    parser.add_argument("--port", "-p", default=None, help="Porta serial. Se omitida, a TUI abre o seletor de portas")
    parser.add_argument("--baud", "-b", type=int, default=115200, help="Baudrate nominal da USB serial")
    parser.add_argument(
        "--mode",
        choices=("auto", "gateway", "sensor"),
        default="auto",
        help="Força o protocolo ou usa detecção automática",
    )
    parser.add_argument("--demo", action="store_true", help="Inicia sem hardware com uma rede simulada")
    parser.add_argument("--no-file-log", action="store_true", help="Desativa o arquivo JSONL automático")
    parser.add_argument(
        "--security-mode",
        choices=("off", "presence", "otp"),
        default="presence",
        help="Proteção operacional: off, presence (YubiKey USB presente) ou otp",
    )
    parser.add_argument(
        "--security-config",
        default=None,
        help="Arquivo JSON de segurança local (device_admin_token, permissões 0600)",
    )
    args = parser.parse_args()

    PicoTuiApp(
        port=args.port,
        baudrate=args.baud,
        mode=args.mode,
        demo=args.demo,
        enable_file_log=not args.no_file_log,
        security_mode=args.security_mode,
        security_config=args.security_config,
    ).run()


if __name__ == "__main__":
    main()
