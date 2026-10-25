#!/usr/bin/env python3
"""Inicialização em um comando da bancada IoT-over-CAN (Linux Mint).

Abre terminais separados para Studio, TUI (Probe virtual) e Pico USB-HIL.
Não altera firmware, dados da rede ou credenciais. Uso:
    ./Simulacao/iniciar_laboratorio.py
    ./Simulacao/iniciar_laboratorio.py status
    ./Simulacao/iniciar_laboratorio.py stop
    ./Simulacao/iniciar_laboratorio.py --rebuild
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import re
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SELF = Path(__file__).resolve()
SIM = ROOT / "Simulacao"
TUI_PY = ROOT / "Front" / ".venv" / "bin" / "python"
DEFAULT_USB = "/dev/serial/by-id/usb-Raspberry_Pi_Pico_E6616408430D5239-if00"
SESSION_ID = hashlib.sha256(str(ROOT).encode()).hexdigest()[:14]
BASE = Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local" / "state")))
SESSION = BASE / "iot-over-can" / f"laboratorio-{SESSION_ID}"
PTY_RE = re.compile(r"^PORT=(/dev/pts/[0-9]+)$")
CTRL_RE = re.compile(r"^CONTROL=(/[^\n]+/control\.sock)$")
COMPONENTS = ("studio", "tui", "usb")


def atomic_write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".{os.getpid()}.tmp")
    tmp.write_text(content, encoding="utf-8")
    os.replace(tmp, path)


def parse_studio_line(line: str) -> tuple[str, str] | None:
    line = line.strip()
    match = PTY_RE.fullmatch(line)
    if match:
        return "port", match.group(1)
    match = CTRL_RE.fullmatch(line)
    if match:
        return "control", match.group(1)
    return None


def current_pid(name: str) -> int | None:
    try:
        pid = int((SESSION / f"{name}.pid").read_text().strip())
    except (OSError, ValueError):
        return None
    if pid < 1 or not Path(f"/proc/{pid}").exists():
        return None
    # Só gerenciar filhos da mesma bancada, evitando atingir PID reutilizado.
    try:
        environ = (Path(f"/proc/{pid}/environ").read_bytes()).split(b"\0")
    except (OSError, PermissionError):
        return None
    expected = f"IOC_LAB_SESSION_ID={SESSION_ID}".encode()
    return pid if expected in environ else None


def stop_session() -> None:
    active = {name: current_pid(name) for name in COMPONENTS}
    if not any(active.values()):
        print("Nenhuma bancada gerenciada por este script está ativa.")
        return
    for name in ("usb", "tui", "studio"):
        pid = active[name]
        if pid is None:
            continue
        try:
            os.kill(pid, signal.SIGTERM)
            print(f"[laboratorio] Encerrando {name}: PID={pid}")
        except ProcessLookupError:
            pass
    # Não apaga a sessão até que os processos terminem; preserva os logs.


def session_status() -> None:
    print(f"Sessão: {SESSION}")
    for name in COMPONENTS:
        pid = current_pid(name)
        print(f"  {name:<8} " + (f"ativo (PID {pid})" if pid else "desligado"))
    for key in ("port", "control"):
        path = SESSION / key
        if path.exists():
            print(f"  {key:<8} {path.read_text(encoding='utf-8').strip()}")
    print(f"  log      {SESSION / 'studio.log'}")


def check_usb_file(device: str, token_file: Path) -> None:
    if not Path(device).exists():
        raise RuntimeError(f"Pico USB não encontrado em {device}. Conecte-o ou use --no-usb.")
    try:
        info = token_file.stat()
    except OSError as exc:
        raise RuntimeError(f"Token administrativo ausente em {token_file}") from exc
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise RuntimeError(f"O token {token_file} deve ser arquivo regular do usuário, com permissões 0600.")


def pick_terminal(requested: str | None = None) -> str:
    supported = ("gnome-terminal", "mate-terminal", "xfce4-terminal", "konsole", "xterm")
    if requested:
        base = Path(requested).name
        if base not in supported or not shutil.which(requested):
            raise RuntimeError(f"Terminal não suportado: {requested}; use {', '.join(supported)}")
        return requested
    for name in supported:
        if shutil.which(name):
            return name
    raise RuntimeError("Nenhum terminal gráfico suportado encontrado. Instale mate-terminal, gnome-terminal, xfce4-terminal, konsole ou xterm.")


def terminal_command(terminal: str, title: str, command: list[str]) -> list[str]:
    name = Path(terminal).name
    if name in ("mate-terminal", "gnome-terminal"):
        return [terminal, f"--title={title}", "--", *command]
    if name == "xfce4-terminal":
        return [terminal, "--disable-server", f"--title={title}", f"--command={shlex.join(command)}"]
    if name == "konsole":
        return [terminal, "--title", title, "-e", *command]
    if name == "xterm":
        return [terminal, "-T", title, "-e", *command]
    raise ValueError(f"Terminal desconhecido: {name}")


def simulator_needs_build(binary: Path) -> bool:
    if not binary.is_file() or not os.access(binary, os.X_OK):
        return True
    stamp = binary.stat().st_mtime
    roots = (ROOT / "Codigo" / "node-can" / "src", ROOT / "Codigo" / "node-can" / "include",
             ROOT / "Codigo" / "node-can" / "test" / "host", ROOT / "Codigo" / "common" / "ioc_link")
    suffixes = {".c", ".cpp", ".h", ".hpp", ".ino", ".inc", ".py"}
    for folder in roots:
        if not folder.is_dir():
            continue
        for path in folder.rglob("*"):
            if path.is_file() and path.suffix in suffixes and path.stat().st_mtime > stamp:
                return True
    for p in (ROOT / "Codigo" / "scripts" / "build_network_sim.sh", SIM / "gen_sim_factories.py"):
        if p.exists() and p.stat().st_mtime > stamp:
            return True
    return False


def run_studio(config: dict) -> int:
    """Executado em terminal próprio; repassa os sinais ao Studio e ao simulador."""
    command = [sys.executable, "-u", str(SIM / "studio" / "server.py"),
               "--binary", config["binary"], "--port", str(config["studio_port"])]
    if config["browser"]:
        command.append("--browser")
    atomic_write(SESSION / "studio.pid", str(os.getpid()))
    proc = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1, start_new_session=True)
    interrupted = False

    def shutdown(_signum, _frame) -> None:
        nonlocal interrupted
        if interrupted:
            return
        interrupted = True
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGINT)
            except ProcessLookupError:
                pass

    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, shutdown)
    try:
        assert proc.stdout is not None
        with (SESSION / "studio.log").open("w", encoding="utf-8", buffering=1) as log:
            for line in proc.stdout:
                log.write(line)
                print(line, end="", flush=True)
                result = parse_studio_line(line)
                if result:
                    key, value = result
                    atomic_write(SESSION / key, value + "\n")
        return proc.wait()
    finally:
        if proc.poll() is None:
            shutdown(signal.SIGTERM, None)
            try:
                proc.wait(timeout=6)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
        (SESSION / "studio.pid").unlink(missing_ok=True)


def run_terminal_component(component: str, config: dict) -> int:
    py = str(TUI_PY)
    if component == "tui":
        port = (SESSION / "port").read_text(encoding="utf-8").strip()
        args = [py, "-m", "pico_tui", "--port", port, "--mode", "probe", "--security-mode", config["security_mode"]]
    elif component == "usb":
        control = (SESSION / "control").read_text(encoding="utf-8").strip()
        args = [py, str(SIM / "usb_sensor_bridge.py"), "--port", config["device"],
                "--socket", control, "--node", str(config["node"]),
                "--rssi", str(config["rssi"]), "--period-ms", str(config["period_ms"]),
                "--token-file", config["token_file"]]
    else:
        raise ValueError(component)
    atomic_write(SESSION / f"{component}.pid", str(os.getpid()))
    print(f"[laboratorio] Iniciando {component}...", flush=True)
    os.execv(py, args)
    return 0


def studio_ready(timeout: float = 25.0) -> tuple[str, str]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        p, s = SESSION / "port", SESSION / "control"
        if p.exists() and s.exists():
            port, control = p.read_text().strip(), s.read_text().strip()
            if PTY_RE.fullmatch("PORT=" + port) and Path(port).exists() and Path(control).is_socket():
                return port, control
        if not current_pid("studio") and (SESSION / "studio.pid").exists():
            raise RuntimeError("O Studio terminou durante a inicialização. Consulte studio.log.")
        time.sleep(0.2)
    raise RuntimeError(f"Studio não publicou PORT/CONTROL em {timeout:g} s. Verifique {SESSION / 'studio.log'}")


def launch(config: dict, args: argparse.Namespace) -> None:
    if any(current_pid(n) for n in COMPONENTS):
        raise RuntimeError("Uma bancada gerenciada já está ativa. Use 'status' ou 'stop'.")
    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY") and not args.dry_run:
        raise RuntimeError("Sessão gráfica indisponível (DISPLAY/WAYLAND_DISPLAY).")
    if not args.dry_run and not (SIM / "studio" / "server.py").is_file():
        raise RuntimeError(f"Execute dentro do repositório com Studio: {ROOT}")
    if not TUI_PY.is_file() and not args.dry_run:
        raise RuntimeError(f"Python da TUI ausente: {TUI_PY}. Execute ./Codigo/scripts/setup_tui.sh")
    if config["usb"] and not args.dry_run:
        check_usb_file(config["device"], Path(config["token_file"]))
    terminal = pick_terminal(args.terminal) if not args.dry_run else args.terminal or "gnome-terminal"
    if args.dry_run:
        print("[dry-run] Não inicia processos nem altera arquivos.")
        for name in COMPONENTS:
            if name == "usb" and not config["usb"]:
                continue
            cmd = [sys.executable, str(SELF), "__run", name]
            print(name + ": " + shlex.join(terminal_command(terminal, "IoT-over-CAN " + name, cmd)))
        print("Porta PTY e socket serão lidos automaticamente do Studio.")
        return
    binary = Path(config["binary"])
    if args.rebuild or simulator_needs_build(binary):
        print("[laboratorio] Compilando/recompilando simulador nativo...")
        subprocess.run(["bash", str(ROOT / "Codigo" / "scripts" / "build_network_sim.sh"), str(binary)],
                       check=True, cwd=ROOT)
    if not binary.exists():
        raise RuntimeError("Compilação não gerou o binário do simulador")
    with socket.socket() as sock:
        if sock.connect_ex(("127.0.0.1", config["studio_port"])) == 0:
            raise RuntimeError(f"Porta HTTP {config['studio_port']} ocupada. Encerre outro Studio ou use --studio-port.")
    SESSION.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(SESSION, 0o700)
    for name in ("port", "control", "studio.log", "studio.pid", "tui.pid", "usb.pid"):
        (SESSION / name).unlink(missing_ok=True)
    atomic_write(SESSION / "config.json", json.dumps(config, ensure_ascii=False, indent=2))
    os.chmod(SESSION / "config.json", 0o600)

    def spawn(name: str) -> None:
        env = dict(os.environ)
        env["IOC_LAB_SESSION_ID"] = SESSION_ID
        cmd = ["/usr/bin/env", f"IOC_LAB_SESSION_ID={SESSION_ID}", sys.executable, str(SELF), "__run", name]
        subprocess.Popen(terminal_command(terminal, f"IoT-over-CAN | {name.upper()}", cmd), cwd=ROOT,
                         env=env, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    spawn("studio")
    try:
        port, control = studio_ready()
    except Exception:
        print("[laboratorio] Studio não iniciou; consulte o log.", file=sys.stderr)
        raise
    print(f"[laboratorio] Studio: http://127.0.0.1:{config['studio_port']}")
    print(f"[laboratorio] TUI conectará em {port}")
    print(f"[laboratorio] Ponte usará socket {control}")
    spawn("tui")
    if config["usb"]:
        spawn("usb")
    print("[laboratorio] Terminais abertos. Na TUI: F7 para associar o Pico; depois abra o Espectro.")
    print(f"[laboratorio] Para encerrar: {shlex.quote(str(SELF))} stop")


def launch_single_instance(config: dict, args: argparse.Namespace) -> None:
    """Serializa partidas; uma segunda chamada nunca observa a sessão vazia."""
    SESSION.parent.mkdir(parents=True, exist_ok=True)
    lock_path = SESSION.parent / (SESSION.name + ".lock")
    fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise RuntimeError("Esta bancada já está sendo iniciada em outro processo.") from exc
        launch(config, args)
    finally:
        os.close(fd)


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description="Inicia Studio, TUI e Pico USB em 1 comando (Linux Mint)")
    ap.add_argument("action", nargs="?", default="start", choices=("start", "stop", "status", "__run"))
    ap.add_argument("component", nargs="?", choices=COMPONENTS, help=argparse.SUPPRESS)
    ap.add_argument("--device", default=DEFAULT_USB, help="Porta física USB do Pico")
    ap.add_argument("--token-file", default="~/.config/ioc-usb-admin-token", help="Caminho do token, nunca seu conteúdo")
    ap.add_argument("--binary", default="/tmp/ioc_sim_network", help="Simulador compilado")
    ap.add_argument("--studio-port", type=int, default=8765, help="Porta HTTP local do Studio")
    ap.add_argument("--node", type=int, default=1, help="Node virtual que observa o Pico")
    ap.add_argument("--rssi", type=int, default=-55, help="RSSI emulado")
    ap.add_argument("--period-ms", type=int, default=1000, help="Período de telemetria USB")
    ap.add_argument("--security-mode", choices=("off", "presence", "otp"), default="off", help="Segurança da TUI")
    ap.add_argument("--terminal", help="Terminal gráfico (gnome-terminal/mate-terminal/xfce4-terminal/konsole/xterm)")
    ap.add_argument("--no-usb", action="store_true", help="Studio+TUI sem ponte física")
    ap.add_argument("--no-browser", action="store_true", help="Não abrir navegador automaticamente")
    ap.add_argument("--rebuild", action="store_true", help="Força compilação do simulador")
    ap.add_argument("--dry-run", action="store_true", help="Mostra o plano, sem iniciar")
    return ap


def main(argv: list[str] | None = None) -> int:
    ap = parser()
    args = ap.parse_args(argv)
    try:
        if args.action == "stop":
            stop_session()
            return 0
        if args.action == "status":
            session_status()
            return 0
        if args.action == "__run":
            if not args.component:
                ap.error("__run exige um componente")
            config = json.loads((SESSION / "config.json").read_text(encoding="utf-8"))
            if args.component == "studio":
                return run_studio(config)
            return run_terminal_component(args.component, config)
        if not (1 <= args.node <= 31 and -127 <= args.rssi <= 0 and 1024 <= args.studio_port <= 65535
                and 100 <= args.period_ms <= 60000):
            ap.error("--node, --rssi, --studio-port ou --period-ms fora dos limites")
        config = {
            "browser": not args.no_browser,
            "usb": not args.no_usb,
            "binary": str(Path(args.binary).expanduser().resolve()),
            "studio_port": args.studio_port,
            "device": args.device,
            "token_file": str(Path(args.token_file).expanduser().resolve()),
            "node": args.node,
            "rssi": args.rssi,
            "period_ms": args.period_ms,
            "security_mode": args.security_mode,
        }
        if args.dry_run:
            launch(config, args)
        else:
            launch_single_instance(config, args)
        return 0
    except (RuntimeError, OSError, subprocess.CalledProcessError) as exc:
        print(f"ERRO: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
