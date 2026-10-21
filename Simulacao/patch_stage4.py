#!/usr/bin/env python3
"""Adiciona inicialização pausada determinística ao motor das etapas 2-3.

Idempotente. Em caso de ancoragem imprevista, não escreve alterações.
"""
from pathlib import Path
import sys

p=Path(sys.argv[1])
s=p.read_text(encoding='utf-8')
if 'bool startPaused = false' in s:
    print('Etapa 4 já presente em sim_interactive.inc')
    raise SystemExit(0)
needle='int simInteractiveLoop() {'
assert s.count(needle)==1, 'modo interativo não localizado'
s=s.replace(needle,'int simInteractiveLoop(bool startPaused = false) {',1)
# Semente ajustável somente quando a simulação está pausada.
needle='    if (action == "PAUSE") {'
assert s.count(needle)==1, 'bloco de comando PAUSE ausente'
s=s.replace(needle,'''    if (action == "SEED") {
      std::string token, extra;
      if (!(in >> token) || (in >> extra)) throw std::runtime_error("uso: SEED <0..4294967295>");
      if (running) throw std::runtime_error("PAUSE antes de SEED");
      g_rng = static_cast<uint32_t>(simParseUnsigned(token, UINT32_MAX));
      printf("#CTRL OK SEED value=%u\\n", g_rng);
      return;
    }
'''+needle,1)
needle='  puts("#CTRL HELP STATUS | PAUSE | RESUME | STEP <ms> | QUIT");'
assert s.count(needle)==1,'ajuda STATUS ausente'
s=s.replace(needle,needle+'\n  puts("#CTRL HELP SEED <uint32> (apenas pausado; altera eventos futuros)");'.replace('\n','\n'),1)
needle='  bool running = true;'
assert s.count(needle)==1, 'estado de execução ambíguo'
s=s.replace(needle,'  bool running = !startPaused;',1)
p.write_text(s,encoding='utf-8')
print('sim_interactive.inc habilita início pausado')
