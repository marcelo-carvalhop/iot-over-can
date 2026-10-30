"""Linha de comando: planejar, coletar, analisar, verificar e medir a criptografia.

  python -m iocpesquisa planejar  desenhos/bancada_virtual.json
  python -m iocpesquisa coletar   desenhos/bancada_virtual.json --binario /tmp/sim_network -j 4
  python -m iocpesquisa coletar   desenhos/bancada_fisica.json  --porta /dev/ttyUSB0
  python -m iocpesquisa analisar  resultados/bancada_virtual
  python -m iocpesquisa verificar resultados/bancada_virtual
  python -m iocpesquisa bench     --porta /dev/ttyACM0 --placa sensor --saida resultados/bancada_fisica
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path

from iocpesquisa import coleta, desenho, relatorio
from iocpesquisa.proveniencia import raiz_repositorio

SAIDA_PADRAO = Path("resultados")


def _planejar(args: argparse.Namespace) -> int:
    d = desenho.carregar(args.desenho)
    p = desenho.plano(d)
    blocos = Counter(e["bloco"] for e in p["execucoes"])
    print(f"desenho {p['desenho']} (sha256 {p['desenho_sha256'][:16]}): {p['condicoes']} condições, "
          f"{len(p['execucoes'])} execuções")
    for bloco, n in blocos.items():
        dur = next(e["parametros"].get("duration_s", 60) + e["parametros"].get("warmup_s", 10)
                   for e in p["execucoes"] if e["bloco"] == bloco)
        print(f"  {bloco}: {n} execuções, ~{n * (dur + 15) / 3600:.1f} h na bancada física")
    if args.saida:
        base, _, _ = coleta.preparar(d, args.saida)
        print(f"plano gravado em {base / 'plano.json'}")
    return 0


def _coletar(args: argparse.Namespace) -> int:
    d = desenho.carregar(args.desenho)
    if bool(args.binario) == bool(args.porta):
        print("informe --binario (bancada virtual) ou --porta (Probe 00 física)", file=sys.stderr)
        return 2
    if args.binario:
        r = coleta.executar(d, args.saida, binario=Path(args.binario), paralelo=args.paralelo, limite=args.limite)
    else:
        try:
            import serial  # pyserial (dependência da TUI)
        except ImportError:
            print("pyserial ausente: use o Python da TUI (Front/.venv/bin/python)", file=sys.stderr)
            return 2
        with serial.Serial(args.porta, args.baud, timeout=0.05) as porta:
            time.sleep(0.5)
            porta.reset_input_buffer()
            r = coleta.executar(d, args.saida, porta=porta, limite=args.limite)
    print(json.dumps(r, ensure_ascii=False))
    return 0 if r.get("erro", 0) == 0 else 1


def _analisar(args: argparse.Namespace) -> int:
    r = relatorio.analisar(Path(args.pasta), figuras=not args.sem_figuras)
    print(json.dumps(r, ensure_ascii=False))
    return 0


def _verificar(args: argparse.Namespace) -> int:
    problemas = relatorio.verificar(Path(args.pasta))
    for p in problemas:
        print("PROBLEMA", p)
    print("nenhum problema encontrado" if not problemas else f"{len(problemas)} problema(s)")
    return 0 if not problemas else 1


def _bench(args: argparse.Namespace) -> int:
    destino = Path(args.saida) / "bench"
    destino.mkdir(parents=True, exist_ok=True)
    carimbo = time.strftime("%Y%m%d-%H%M%S")
    linhas: list[str] = []
    if not args.host and not args.porta:
        print("informe --porta (placa) ou --host", file=sys.stderr)
        return 2
    if args.host:
        raiz = raiz_repositorio()
        link = raiz / "Codigo/common/ioc_link"
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "bench"
            subprocess.run(["cc", "-std=c11", "-O2", "-D_POSIX_C_SOURCE=200809L", f"-I{link}",
                            str(link / "ioc_sha256.c"), str(link / "ioc_link.c"), str(link / "ioc_bench.c"),
                            str(raiz / "Codigo/common/tests/test_ioc_bench.c"), "-o", str(exe)], check=True)
            linhas = subprocess.run([str(exe), "--host"], capture_output=True, text=True, check=True).stdout.splitlines()
        placa = "host"
    else:
        import serial
        placa = args.placa
        with serial.Serial(args.porta, args.baud, timeout=0.1) as porta:
            time.sleep(0.5)
            porta.reset_input_buffer()
            porta.write(f"BENCH CRYPTO {args.n}\n".encode())
            fim = time.monotonic() + 30
            buffer = b""
            while time.monotonic() < fim:
                buffer += porta.read(4096)
                if b"BENCH_DONE" in buffer:
                    break
            linhas = [x.strip() for x in buffer.decode("utf-8", "replace").splitlines() if "BENCH" in x]
    caminho = destino / f"{placa}-{carimbo}.jsonl"
    with caminho.open("w", encoding="utf-8") as f:
        f.write(json.dumps({"tipo": "cabecalho", "placa": placa, "registrado_em": carimbo}) + "\n")
        for linha in linhas:
            f.write(json.dumps({"tipo": "linha", "fonte": "probe", "t_ref_ms": 0, "t_host_ns": None, "linha": linha}) + "\n")
            print(linha)
    print(f"gravado em {caminho}")
    return 0 if any("op=" in x for x in linhas) else 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="iocpesquisa", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("planejar", help="expande o desenho e mostra (ou grava) o plano")
    p.add_argument("desenho")
    p.add_argument("--saida", type=Path)
    p.set_defaults(func=_planejar)
    p = sub.add_parser("coletar", help="executa as execuções pendentes")
    p.add_argument("desenho")
    p.add_argument("--saida", type=Path, default=SAIDA_PADRAO)
    p.add_argument("--binario", help="bancada virtual (Codigo/scripts/build_network_sim.sh <saida>)")
    p.add_argument("--porta", help="porta serial da Probe 00")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("-j", "--paralelo", type=int, default=1)
    p.add_argument("--limite", type=int, help="no máximo N execuções nesta chamada")
    p.set_defaults(func=_coletar)
    p = sub.add_parser("analisar", help="métricas, tabelas, comparações e figuras")
    p.add_argument("pasta")
    p.add_argument("--sem-figuras", action="store_true")
    p.set_defaults(func=_analisar)
    p = sub.add_parser("verificar", help="confere a reprodutibilidade da campanha")
    p.add_argument("pasta")
    p.set_defaults(func=_verificar)
    p = sub.add_parser("bench", help="custo da criptografia numa placa (BENCH CRYPTO) ou no computador")
    p.add_argument("--porta")
    p.add_argument("--placa", choices=("probe", "node", "sensor"), default="node")
    p.add_argument("--host", action="store_true")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("-n", type=int, default=200)
    p.add_argument("--saida", type=Path, default=SAIDA_PADRAO, help="pasta da campanha (grava em <saida>/bench)")
    p.set_defaults(func=_bench)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
