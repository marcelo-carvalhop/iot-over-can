"""Transformacoes conservadoras em sim_network.cpp (apos a etapa 1)."""
from pathlib import Path
import sys

p=Path(sys.argv[1]);s=p.read_text(encoding='utf-8')
if '#include "sim_factory_decls.inc"' in s:
    print('sim_network.cpp ja inclui o gerenciador dinamico; mantido')
    sys.exit(0)
replacements=[
('::SimInstance* sim_make_n3b();\n','::SimInstance* sim_make_n3b();\n#include "sim_factory_decls.inc"\n'),
('    setupBench();\n    formNetwork();\n    return simInteractiveLoop();',
 '    setupBench();\n    simPrepareFactories();\n    formNetwork();\n    return simInteractiveLoop();'),
('Node& probe() { return node(0); }',
 '''// Nodes removidos continuam podendo figurar nas observacoes radio armazenadas.
Node* simTryNode(uint8_t id) {
  for (Node& n : g_nodes) if (n.id == id) return &n;
  return nullptr;
}

Node& probe() { return node(0); }'''),
('''    Node& n = node(parent);
    if (!n.powered || !n.inst) return false;''',
 '''    Node* n = simTryNode(parent);
    if (!n || !n->powered || !n->inst) return false;'''),
('''    if (!n.inst->apInfo(ssid, psk, channel)) return false;''',
 '''    if (!n->inst->apInfo(ssid, psk, channel)) return false;'''),
('''        Node& n = node(entry.first);
        if (n.powered && n.inst) n.inst->bleDeliver(mfg, entry.second);''',
 '''        Node* n = simTryNode(entry.first);
        if (n && n->powered && n->inst) n->inst->bleDeliver(mfg, entry.second);'''),
('''        Node& n = node(entry.first);
        if (!n.powered || !n.inst) continue;
        std::string mfg;
        if (!n.inst->bleOffer(mfg)) continue;''',
 '''        Node* n = simTryNode(entry.first);
        if (!n || !n->powered || !n->inst) continue;
        std::string mfg;
        if (!n->inst->bleOffer(mfg)) continue;'''),
]
# Condicoes de radio e processamento usadas apenas no modo interativo.
replacements.extend([
    ('  uint32_t restarts = 0;\n};',
     '  uint32_t restarts = 0;\n  uint8_t cpuBudgetPct = 100;  // fração de passos do loop atendidos (modelo)\n};'),
    ('struct Sensor {', '''// RSSI e uma observacao; perdas sao parametros independentes.
std::map<std::pair<uint64_t,uint8_t>,uint8_t> g_radio_loss_pct;
bool simRadioDrop(uint64_t uuid,uint8_t id) {
  if (!g_interactive_runtime) return false;
  auto it=g_radio_loss_pct.find({uuid,id});
  return it!=g_radio_loss_pct.end() && it->second>0 && sim_random32()%100u<it->second;
}

struct Sensor {'''),
    ('    node(parent).inst->udpDeliver(d);',
     '    if (!simRadioDrop(uuid,parent)) node(parent).inst->udpDeliver(d);'),
    ('        if (n && n->powered && n->inst) n->inst->bleDeliver(mfg, entry.second);',
     '        if (n && n->powered && n->inst && !simRadioDrop(uuid,entry.first)) n->inst->bleDeliver(mfg, entry.second);'),
    ('        if (!n->inst->bleOffer(mfg)) continue;',
     '        if (!n->inst->bleOffer(mfg) || simRadioDrop(uuid,entry.first)) continue;'),
    ('        if (s.poweredOn && s.parent == n.id && s.port == d.port && s.linkUp()) s.onDatagram(d);',
     '        if (s.poweredOn && s.parent == n.id && s.port == d.port && s.linkUp() && !simRadioDrop(s.uuid,n.id)) s.onDatagram(d);'),
    ('''    if (!n.powered || !n.inst) continue;
    try {
      n.inst->step();''',
     '''    if (!n.powered || !n.inst) continue;
    // Reducao sintetica de capacidade; nao representa medicao fisica de CPU.
    if (g_interactive_runtime && n.id != 0 && n.cpuBudgetPct < 100 &&
        sim_random32()%100u >= n.cpuBudgetPct) continue;
    try {
      n.inst->step();'''),
])
for a,b in replacements:
    count=s.count(a)
    if count!=1: raise SystemExit(f'Trecho do firmware nao encontrado exatamente uma vez (ocorrencias={count}): {a[:110]!r}. Nenhum arquivo gravado.')
    s=s.replace(a,b,1)
p.write_text(s,encoding='utf-8')
print('sim_network.cpp atualizado para pool configuravel de Nodes e remocao segura')
