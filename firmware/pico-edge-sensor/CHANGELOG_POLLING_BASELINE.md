# Baseline polling-only

## Decisão de arquitetura

O modo INT/DRDY foi desativado como caminho principal desta versão. A aquisição passa a ser feita por polling I2C de forma explícita e permanente.

## Motivo

Nos testes de bancada, o DRDY funcionou, mas alternou repetidamente entre:

```text
ACQ=DRDY
ACQ=POLLING
INFO DRDY_RECOVERY_ATTEMPT
```

Isso cria complexidade desnecessária para fechar o ciclo do sensor, do gateway e da TUI.

## Nova política

- `ACQ=POLLING` é o modo oficial.
- INT/DRDY não é habilitado.
- `DRDY_IRQ` deve permanecer 0.
- `DRDY_MISSED` deve permanecer 0.
- Não há tentativa automática de recuperação DRDY.
- O comando `ACQ DRDY` é rejeitado.
- O comando correto é:

```text
ACQ POLLING
```

## Consequência

A captura fica menos elegante do ponto de vista temporal do que DRDY, mas fica mais previsível e mais simples para a baseline acadêmica atual.

Para pesquisa futura, DRDY pode ser reintroduzido como versão experimental quando houver PCB, barramento I2C mais estável e menos ruído de bancada.
