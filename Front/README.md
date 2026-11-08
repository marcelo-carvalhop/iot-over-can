# iot-over-can: estação de operação

Programa em tela de terminal (Python, Textual) para supervisionar, comandar e medir a rede do [iot-over-can](https://github.com/marcelo-carvalhop/iot-over-can).

```bash
./Codigo/scripts/setup_tui.sh                # na raiz do repositório; cria Front/.venv
Front/.venv/bin/iot-over-can-tui --demo      # rede de demonstração, sem hardware
./Codigo/scripts/run_tui.sh /dev/ttyUSB0     # com a Probe 00
./Codigo/scripts/test_tui.sh                 # testes
```

Telas, teclas e arquivos: [`Documentacao/tui.md`](../Documentacao/tui.md). Organização do código: [`Documentacao/integracao.md`](../Documentacao/integracao.md). Licença: GNU GPL versão 3.
