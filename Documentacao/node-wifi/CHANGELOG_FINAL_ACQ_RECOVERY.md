# Finalização: recuperação DRDY e saída do fallback polling

## Problema observado

Após a versão robusta entrar em fallback, o firmware permanecia em:

```text
ACQ=POLLING
DRDY_IRQ=0
WARN STILL_USING_I2C_POLLING_MODE DRDY_BUFFER_NOT_READY
```

Mesmo com o MPU funcionando, não havia um caminho limpo para religar o INT/DRDY depois que `mpu6050_enter_polling_mode()` desabilitava a interrupção.

## Correções

- Adicionada função `main_restart_drdy_acquisition()`.
- Ao aplicar configuração, se o estado anterior era `ACQ=POLLING`, o firmware agora chama `mpu6050_start_acquisition()` em vez de apenas recalcular divisor.
- Adicionado comando serial:

```text
ACQ DRDY
```

ou

```text
ACQ RESET
```

- Adicionada recuperação automática: a cada 15 s em `ACQ=POLLING`, o firmware tenta religar DRDY.
- Quando ocorre tentativa automática, a serial informa:

```text
INFO DRDY_RECOVERY_ATTEMPT
```

## Critério de aceite

Depois de `ACQ DRDY` ou de uma tentativa automática bem-sucedida:

```text
STATUS
```

deve mostrar:

```text
ACQ=DRDY
DRDY_IRQ>0
DRDY_MISSED=0
```


## Revisão adicional

- `main_restart_drdy_acquisition()` agora cria uma janela de graça baseada no tamanho do buffer e na taxa efetiva.
- Isso impede que o firmware religue DRDY e caia imediatamente de volta para polling antes de haver tempo físico para fechar um buffer.
- O fallback só pode disparar depois dessa janela de graça.
