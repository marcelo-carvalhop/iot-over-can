"""Modifica apenas o ponto de entrada da TUI, preservando o dominio e os protocolos."""
from pathlib import Path
import sys

p=Path(sys.argv[1]);s=p.read_text(encoding='utf-8')
if '"--sim"' in s:
    print('TUI ja possui --sim; preservada')
    sys.exit(0)
add='''    parser.add_argument("--sim", action="store_true", help="Inicia uma Probe virtual e conecta a TUI automaticamente")
    parser.add_argument("--sim-binary", default="/tmp/ioc_sim_network", help="Caminho para o simulador nativo compilado")
    parser.add_argument("--sim-record", default=None, help="Grava a sessao virtual em formato replay")
'''
anchor='    args = parser.parse_args()\n'
assert s.count(anchor)==1
s=s.replace(anchor,add+'''    args = parser.parse_args()
    if args.sim:
        if args.port or args.demo or args.replay:
            parser.error("--sim nao pode ser combinado com --port, --demo ou --replay")
        if args.mode not in ("auto", "probe"):
            parser.error("--sim exige modo de protocolo Probe")
''')
s=s.replace('import argparse\n','import argparse\nimport sys\nfrom pathlib import Path\nfrom contextlib import nullcontext\n')
s=s.replace('        default="presence",\n        help="Proteção operacional:', '        default=None,\n        help="Proteção operacional:')
needle='''    PicoTuiApp(
        port=args.port,
'''
replace='''    session = nullcontext(None)
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
'''
assert s.count(needle)==1
s=s.replace(needle,replace)
# Indent contents from 'baudrate=' to '.run()' one level inside 'with'
start=s.index('        port=args.port,\n',s.index('    with session as simulated_port:'))
stop=s.index('    ).run()\n',start)+len('    ).run()\n')
part=s[start:stop]
s=s[:start] + ''.join(('    '+line if line.strip() else line) for line in part.splitlines(True)) + s[stop:]
s=s.replace('security_mode=args.security_mode,','security_mode=args.security_mode or ("off" if args.sim else "presence"),')
p.write_text(s,encoding='utf-8')
print('TUI integrada ao --sim:',p)
