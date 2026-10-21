from __future__ import annotations

import argparse
import sys
from pathlib import Path
from contextlib import nullcontext

from pico_tui.app import PicoTuiApp


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="iot-over-can-tui",
        description="Console de operação da rede iot-over-can (módulos CAN, Probe 00 e sensores sem fio).",
    )
    parser.add_argument("--port", "-p", default=None, help="Porta serial. Se omitida, a TUI abre o seletor de portas")
    parser.add_argument("--baud", "-b", type=int, default=115200, help="Baudrate nominal da USB serial")
    parser.add_argument(
        "--mode",
        choices=("auto", "probe", "gateway", "sensor"),
        default="auto",
        help="Força o protocolo (probe/gateway ou sensor) ou usa detecção automática",
    )
    parser.add_argument("--demo", action="store_true", help="Inicia sem hardware com uma rede simulada")
    parser.add_argument(
        "--replay",
        default=None,
        metavar="ARQUIVO",
        help="Reproduz uma gravação das linhas da Probe 00 (por exemplo, Front/replays/owner_failover.log)",
    )
    parser.add_argument(
        "--replay-speed",
        type=float,
        default=1.0,
        metavar="FATOR",
        help="Velocidade da reprodução (2 = duas vezes mais rápido)",
    )
    parser.add_argument("--no-file-log", action="store_true", help="Desativa o arquivo JSONL automático")
    parser.add_argument("--record-session", action="store_true",
                        help="Habilita a gravação serial integral para diagnóstico/replay (pode ocupar muito disco)")
    parser.add_argument(
        "--messages",
        action=argparse.BooleanOptionalAction,
        default=None,
        help="Mostra (ou, com --no-messages, oculta) as mensagens recentes na tela inicial nesta sessão",
    )
    parser.add_argument(
        "--ascii",
        action="store_true",
        default=None,
        help="Usa símbolos ASCII em vez de ● ▲ ■ ○ (consoles sem Unicode)",
    )
    parser.add_argument(
        "--preferences",
        default=None,
        help="Arquivo de preferências da interface (padrão: ~/.config/iot-over-can/tui.json)",
    )
    parser.add_argument(
        "--security-mode",
        choices=("off", "presence", "otp"),
        default=None,
        help="Proteção operacional: off, presence (YubiKey USB presente) ou otp",
    )
    parser.add_argument(
        "--security-config",
        default=None,
        help="Arquivo JSON de segurança local (device_admin_token, permissões 0600)",
    )
    parser.add_argument("--sim", action="store_true", help="Inicia uma Probe virtual e conecta a TUI automaticamente")
    parser.add_argument("--sim-binary", default="/tmp/ioc_sim_network", help="Caminho para o simulador nativo compilado")
    parser.add_argument("--sim-record", default=None, help="Grava a sessao virtual em formato replay")
    args = parser.parse_args()
    if args.sim:
        if args.port or args.demo or args.replay:
            parser.error("--sim nao pode ser combinado com --port, --demo ou --replay")
        if args.mode not in ("auto", "probe"):
            parser.error("--sim exige modo de protocolo Probe")

    session = nullcontext(None)
    if args.sim:
        repo_root = Path(__file__).resolve().parents[2]
        sys.path.insert(0, str(repo_root / "Simulacao"))
        from sim_tui_runtime import local_simulation
        session = local_simulation(Path(args.sim_binary), Path(args.sim_record) if args.sim_record else None)

    with session as simulated_port:
        if simulated_port:
            args.port = simulated_port
            args.mode = "probe"
        PicoTuiApp(
            port=args.port,
            baudrate=args.baud,
            mode=args.mode,
            demo=args.demo,
            enable_file_log=not args.no_file_log,
            enable_raw_recording=args.record_session,
            security_mode=args.security_mode or ("off" if args.sim else "presence"),
            security_config=args.security_config,
            show_messages=args.messages,
            ascii_symbols=args.ascii,
            preferences_path=args.preferences,
            replay=args.replay,
            replay_speed=args.replay_speed,
        ).run()


if __name__ == "__main__":
    main()
