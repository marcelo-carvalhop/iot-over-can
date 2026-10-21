#!/usr/bin/env python3
"""Acrescenta --interactive-paused ao executável de simulação, sem alterar cenários."""
from pathlib import Path
import sys
p=Path(sys.argv[1]); s=p.read_text(encoding='utf-8')
if '"--interactive-paused"' in s:
    print('Etapa 4 já presente em sim_network.cpp')
    raise SystemExit(0)
replacement=[
('  bool interactive = false;\n', '  bool interactive = false;\n  bool interactivePaused = false;\n'),
('    else if (strcmp(argv[i], "--interactive") == 0) interactive = true;\n',
 '    else if (strcmp(argv[i], "--interactive") == 0) interactive = true;\n'
 '    else if (strcmp(argv[i], "--interactive-paused") == 0) { interactive = true; interactivePaused = true; }\n'),
('    return simInteractiveLoop();','    return simInteractiveLoop(interactivePaused);')]
for before,after in replacement:
    assert s.count(before)==1, f'âncora não encontrada univocamente: {before!r}'
    s=s.replace(before,after,1)
p.write_text(s,encoding='utf-8')
print('sim_network.cpp habilita --interactive-paused')
