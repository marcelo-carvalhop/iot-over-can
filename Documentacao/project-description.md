# Descrição do projeto

O **iot-over-can** é um projeto experimental de Internet das Coisas embarcada voltado ao estudo, à implementação e à validação de uma rede distribuída baseada em CAN/CAN FD, com suporte a eventos determinísticos, eventos estocásticos, supervisão local, diagnóstico operacional e controle seguro de nós.

Embora o protótipo utilize sensores de vibração como fonte inicial de telemetria, o foco principal do projeto não está na função sensorial em si, mas na construção de uma infraestrutura de comunicação capaz de integrar nós heterogêneos, preservar estados operacionais, tratar falhas e demonstrar compatibilidade entre diferentes modelos de eventos em um ambiente desconectado da Internet.

A proposta parte da ideia de que aplicações IoT não precisam depender necessariamente de arquiteturas centralizadas, brokers externos ou conectividade permanente com a nuvem. Em vez disso, o projeto explora uma abordagem local, fechada e controlada, na qual microcontroladores, sensores e gateways se comunicam por meio de enlaces sem fio locais e de uma rede CAN/CAN FD supervisionada.

Essa arquitetura permite estudar como eventos periódicos, como telemetria, batimentos de controle e confirmações de comando, podem coexistir com eventos imprevisíveis, como falhas, saturações, comandos manuais, diagnósticos e alterações de configuração.

O sistema utiliza um nó sensor baseado em Raspberry Pi Pico 2 W para aquisição e pré-processamento de sinais, inicialmente com um acelerômetro MPU6050. Esse nó atua como gerador de eventos físicos e telemetria resumida, fornecendo dados de vibração, métricas de processamento digital de sinais, códigos de diagnóstico e estados operacionais. A função desse sensor, entretanto, é instrumental: ele serve como carga experimental para validar a rede, os protocolos, a supervisão e a segurança do sistema.

A camada de supervisão é realizada por uma TUI local, que funciona como estação de operação, diagnóstico e controle. A descoberta de sensores wireless pode ocorrer automaticamente, mas a associação operacional a um módulo CAN depende de uma decisão explícita do operador, reforçando a ideia de um sistema offline, controlado e com superfície de ataque reduzida. O uso de uma chave física, como uma YubiKey, adiciona uma camada de proteção operacional para comandos sensíveis, criando uma separação clara entre observação passiva e ações de controle.

No plano da rede, o projeto busca compatibilizar dois regimes de comportamento. O primeiro é determinístico, representado por ciclos de supervisão, mensagens periódicas, comandos com confirmação, janelas de transmissão e estados replicáveis. O segundo é estocástico, representado por eventos físicos, falhas intermitentes, variações de sinal, saturações, perda de comunicação, entrada e saída de nós, atrasos e diagnósticos assíncronos.

Dessa forma, o **iot-over-can** pode ser entendido como uma plataforma de pesquisa aplicada para redes IoT locais, determinísticas e tolerantes a eventos, com ênfase em comunicação embarcada, segurança operacional, diagnóstico distribuído e integração futura com sistemas CAN FD.
