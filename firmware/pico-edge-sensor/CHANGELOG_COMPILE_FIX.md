# Correção de compilação da versão final

## Problemas corrigidos

- `g_drdy_recovery_grace_until_ms` era usado em `main_restart_drdy_acquisition()` e no loop principal, mas não estava declarado globalmente.
- `serial_console.c` chamava `main_restart_drdy_acquisition()` sem protótipo externo, gerando warning de declaração implícita.

## Correção

- Adicionada declaração global:

```c
static volatile uint32_t g_drdy_recovery_grace_until_ms = 0;
```

- Adicionado protótipo em `serial_console.c`:

```c
extern bool main_restart_drdy_acquisition(void);
```
