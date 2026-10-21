#!/usr/bin/env python3
"""Gera fabrica de instancias nativas a partir de um limite de compilacao.

Cada geracao possui globals isolados por namespace. A quantidade de Nodes
presentes na bancada passa a ser definida durante a execucao, mas o numero
maximo de IDs/reinicios precisa ser decidido durante a compilacao.
"""
from __future__ import annotations

import argparse
from pathlib import Path


def factory_name(node: int, generation: int) -> str:
    if node in (1, 2, 3) and generation in (0, 1):
        return f"n{node}{'ab'[generation]}"
    return f"n{node}g{generation}"


def generate(max_nodes: int, generations: int, directory: Path) -> list[tuple[str, int]]:
    if not 3 <= max_nodes <= 31:
        raise ValueError("IOC_SIM_MAX_NODES deve estar entre 3 e 31 (IDs do protocolo CAN)")
    if not 2 <= generations <= 16:
        raise ValueError("IOC_SIM_GENERATIONS deve estar entre 2 e 16")
    directory.mkdir(parents=True, exist_ok=True)
    instances = [("probe", 0)] + [
        (factory_name(n, g), n)
        for n in range(1, max_nodes + 1)
        for g in range(generations)
    ]
    # As sete instancias historicas permanecem disponiveis para os 17 testes.
    extra = [name for name, _ in instances if name not in
             {"probe", "n1a", "n1b", "n2a", "n2b", "n3a", "n3b"}]
    (directory / "sim_factory_decls.inc").write_text(
        "/* Gerado automaticamente. Declaracoes de fabricas adicionais. */\n"
        + "".join(f"::SimInstance* sim_make_{name}();\n" for name in extra), encoding="utf-8")
    lines = [
        "/* Gerado automaticamente. Fabrica com pools por Node. */",
        "std::map<uint8_t, std::vector<std::function<::SimInstance*()>>> simGeneratedFactories() {",
        "  return {",
    ]
    for n in range(1, max_nodes + 1):
        names = ", ".join(f"sim_make_{factory_name(n, g)}" for g in range(generations))
        lines.append(f"    {{{n}, {{{names}}}}},")
    lines.extend(["  };", "}", ""])
    (directory / "sim_factory_registry.inc").write_text("\n".join(lines), encoding="utf-8")
    return instances


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--max-nodes", type=int, default=6)
    p.add_argument("--generations", type=int, default=2)
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    try:
        instances = generate(a.max_nodes, a.generations, a.out)
    except ValueError as exc:
        p.error(str(exc))
    for name, node_id in instances:
        print(f"{name}:{node_id}")


if __name__ == "__main__":
    main()
