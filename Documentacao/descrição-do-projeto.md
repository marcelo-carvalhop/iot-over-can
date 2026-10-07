# Descrição do projeto

O **iot-over-can** é uma plataforma experimental de Internet das Coisas embarcada voltada ao estudo e à validação de uma rede distribuída baseada em CAN/CAN FD, com integração de eventos periódicos, eventos assíncronos, descoberta de dispositivos wireless, supervisão local, diagnóstico e controle seguro.

O sistema não é definido por um único sensor. O sensor de vibração baseado em Raspberry Pi Pico W e MPU6050 é uma carga experimental para produzir telemetria, eventos e diagnósticos. Os módulos ESP32 conectados ao CAN também podem possuir sensores, atuadores e lógica local, além de detectar e assumir responsabilidade por dispositivos wireless.

A **Probe 00** fornece instrumentação entre barramento e computador, sem participar da lógica distribuída. Os **Nodes CAN** formam a rede funcional e podem ser líder ou seguidor. O **Pico W** é um dispositivo wireless autônomo que anuncia sua identidade por BLE.

O comportamento combina mensagens periódicas, como liveness, com eventos imprevisíveis, como alterações de RSSI, falhas, DTCs, saturação, entrada/saída de dispositivos e ações do operador. A convivência entre comunicação determinística do CAN e fenômenos assíncronos do domínio wireless é um dos focos experimentais.

A associação wireless é explícita: múltiplos Nodes podem observar o mesmo UUID, a TUI apresenta as observações, o operador escolhe o responsável e o Node confirma o vínculo. O filho lógico `NN.CC` representa identidade e responsabilidade; não significa que o plano de dados já esteja estabelecido. Telemetria e comandos do Pico W através do Node responsável permanecem uma camada posterior.

A TUI funciona como estação de instrumentação e operação. Sua interface apresenta contexto por telas, centraliza comandos, destaca anomalias e torna descoberta/associação wireless acessível sem exigir comandos ocultos. A aplicação continua operando offline, sem depender de conectividade externa.
