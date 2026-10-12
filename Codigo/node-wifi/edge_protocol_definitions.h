/**
 * @file edge_protocol_definitions.h
 * @brief Definições de estruturas binárias, protocolos de rede UDP e códigos DTC industriais.
 * Atende aos Itens 9 (Anatomia do DTC) e 12 (Arquitetura de Rede Híbrida).
 *
 * NOTA DE VERSIONAMENTO DE PROTOCOLO (v0x05):
 * - Todos os enums usados DENTRO de structs __attribute__((packed)) foram
 *   trocados para uint8_t explícito. Enums em C não têm tamanho garantido
 *   pela linguagem; o tamanho de 1 byte usado aqui era um efeito colateral
 *   de como o GCC do Pico SDK empacota enums pequenos, mas isso NÃO é
 *   portável entre toolchains (ex.: o compilador do ESP32 pode escolher
 *   outro tamanho). Usar uint8_t explícito remove essa ambiguidade — a
 *   conversão para o enum correspondente deve ser feita depois de
 *   deserializar o pacote, nunca antes.
 * - dtc_active_code passou de uint8_t para uint16_t (ver Payload_TelemetryStream).
 * - Payload_ClaimCommand ganhou um campo auth_key (autenticação leve).
 * - CMD_PING / CMD_PONG permanecem como liveness lease de sessão, não como sincronização acadêmica.
 * - Novo par CMD_GET_CONFIG_MENU / CMD_CONFIG_MENU: o nó anuncia os valores
 *   válidos de configuração (modos FSM, janelas, faixa de sample rate) e o
 *   estado atual, tanto no Wi-Fi quanto (em formato texto/JSON equivalente)
 *   na porta serial. Ver README.md, seção "Menu de Configuração".
 * A revisão v0x05 introduz UUID64, token64, contador monotônico de requisição,
 * validação compartilhada e credenciais de compatibilidade fora do código-fonte.
 */
#ifndef EDGE_PROTOCOL_DEFINITIONS_H
#define EDGE_PROTOCOL_DEFINITIONS_H

#include <stdint.h>
#include <stdbool.h>

#define NET_UDP_PORT_DISCOVERY  4242
#define NET_MAGIC_HEADER        0xAA55
// v0x06: todo datagrama passa a trafegar dentro do envelope autenticado
// definido em Codigo/common/ioc_link/ioc_link.h (HMAC-SHA256 com chave por
// dispositivo e chave de sessão). O CLAIM por chave compartilhada de 64 bits
// e o controle UDP sem autenticação da v0x05 foram removidos. As estruturas
// abaixo continuam sendo o conteúdo ("payload interno") do envelope; os
// campos session_token e request_counter permanecem por compatibilidade de
// layout, mas a proteção contra repetição é feita pelo contador do envelope.
#define NET_PROTOCOL_VERSION    0x06

// Tamanho máximo recomendado de payload UDP para evitar fragmentação IP em
// Wi-Fi (MTU típico 1500 - cabeçalhos IP/UDP ~28 bytes = ~1472 bytes úteis).
// edge_net_send_telemetry() usa isso para limitar quantos bins de FFT são
// anexados por pacote.
#define NET_MAX_SAFE_PAYLOAD_BYTES 1472

// Timeout de sessão: se o nó CAN pai não enviar nenhum comando válido
// (CMD_SET_CONFIG ou CMD_PING) dentro desse intervalo, o sensor assume que a
// sessão caiu/reiniciou e volta para NET_STATE_DISCOVERY.
#define NET_SESSION_TIMEOUT_MS   10000
// Tempo máximo em DISCOVERY, após entrar em um AP por oferta, sem conseguir
// autenticar. Depois disso o Wi-Fi é desligado e o sensor volta a aguardar
// uma nova oferta BLE (o Node pode ter reiniciado ou removido o vínculo).
#define NET_DISCOVERY_GIVEUP_MS  60000
// Intervalo entre novas tentativas de associação ao AP quando o rádio
// reporta falha (AP ainda não ativo, senha recusada, fora de alcance).
#define NET_WIFI_REJOIN_MS       5000
// Em DISCOVERY, tempo sem enlace Wi-Fi ativo antes de refazer a associação
// ao AP por completo (o AP do Node pode ter reiniciado).
#define NET_WIFI_LINK_DOWN_MS    10000
// Depois de entrar no AP de um Node por oferta, ofertas de outro Node só são
// seguidas após este tempo. Limita o efeito de uma oferta antiga repetida.
#define NET_OFFER_SWITCH_HOLD_MS 15000
// Intervalo alvo em que o nó CAN pai deveria mandar CMD_PING.
#define NET_LIVENESS_INTERVAL_MS 3000

// ============================================================================
// ITEM 13: MAPEAMENTO ELÉTRICO E PINOUT (RP2350 - PICO 2W)
// ============================================================================
#define PIN_I2C0_SDA            0       // I2C0: Dedicado para DMA e MPU-6050 (Vibração)
#define PIN_I2C0_SCL            1       // Clock Fast-Mode Plus (1 MHz)
#define PIN_MPU_INT_DRDY        2       // Item 10: Gatilho de Hardware (DRDY - 1000 Hz)

#define PIN_I2C1_SDA            6       // I2C1: Dedicado para CPU e MAX17048 (Bateria)
#define PIN_I2C1_SCL            7       // Clock Fast-Mode (400 kHz)
#define PIN_MAX17_ALRT          8       // Interrupção de Bateria Fraca

// ============================================================================
// IDENTIFICADORES DE PERFIL E MÁQUINA DE ESTADOS DO NÓ
// ============================================================================
// Sem __attribute__((packed)) e sem uso direto em structs de rede: são
// usados apenas na lógica interna, após deserialização (ver nota de
// versionamento no topo do arquivo).
typedef enum {
    PROFILE_ACCELEROMETER   = 0x01, // Nó Pico 2W #1: MPU-6050 (Vibração & Sismografia)
    PROFILE_ACOUSTIC_MEMS   = 0x02, // Nó Pico 2W #2: Microfone I2S
    PROFILE_THERMAL_ARRAY   = 0x03  // Nó Pico 2W #3: Sensores PT100/DS18B20
} NodeProfileID;

typedef enum {
    CMD_BEACON_BROADCAST    = 0x01, // reservado (v0x05); substituído pelo HELLO autenticado
    CMD_CLAIM_NODE          = 0x10, // reservado (v0x05); substituído por CHALLENGE/CONFIRM
    CMD_ACK_CAPABILITIES    = 0x11, // RP2350 -> ESP32: "Vínculo aceito. Estrutura anexa"
    CMD_SET_CONFIG          = 0x12, // ESP32 -> RP2350: "Atualize os parâmetros operacionais DSP"
    CMD_ACK_CONFIG          = 0x13, // RP2350 -> ESP32: "Configuração DSP aplicada com sucesso"
    CMD_PING                = 0x14, // ESP32 -> RP2350: liveness/keepalive de sessão
    CMD_PONG                = 0x15, // RP2350 -> ESP32: resposta de liveness
    CMD_GET_CONFIG_MENU     = 0x16, // ESP32 -> RP2350: "Me diga as opções/faixas de config válidas"
    CMD_CONFIG_MENU         = 0x17, // RP2350 -> ESP32: "Aqui estão os valores válidos + config atual"
                                     // (enviado automaticamente logo após CMD_ACK_CAPABILITIES)
    CMD_CLEAR_DTC           = 0x18, // ESP32 -> RP2350: limpa DTCs ativos sob sessão válida
    CMD_DTC_SNAPSHOT        = 0x19, // RP2350 -> ESP32: snapshot compacto da tabela de DTC
    CMD_RELEASE             = 0x1B, // ESP32 -> RP2350: vínculo removido; encerre a sessão e desligue o Wi-Fi
    CMD_ACOUSTIC_CONTROL    = 0x1C, // ESP32 -> RP2350: reinício da medição e assinatura de referência (perfil acústico)
    CMD_ACOUSTIC_CONTROL_ACK= 0x1D, // RP2350 -> ESP32: resultado de CMD_ACOUSTIC_CONTROL
    CMD_TELEMETRY_STREAM    = 0x20, // RP2350 -> ESP32: "Pacote de telemetria contínua / FFT"
    CMD_ACOUSTIC_STREAM     = 0x21, // RP2350 -> ESP32: telemetria acústica (+ espectro opcional)
    CMD_URGENT_DTC_ALARM    = 0xFF  // RP2350 -> ESP32: "Alerta crítico BITE / Falha de hardware"
} NetworkCommandID;

typedef enum {
    FSM_MODE_IDLE           = 0x00, // Baixo consumo, aquisição pausada
    FSM_MODE_ROTATING_MACH  = 0x01, // Máquinas rolantes: RMS AC, Curtose, FFT Pico
    FSM_MODE_STRUCTURAL     = 0x02, // Análise estrutural: Fator de Cresta, Entropia Espectral
    FSM_MODE_SEISMIC_STALTA = 0x03  // Sismografia: Oversampling 18-bit, Trigger STA/LTA
} OperationModeFSM;

// Modos do perfil acústico (PROFILE_ACOUSTIC_MEMS). O campo target_mode de
// Payload_Configuration é interpretado conforme o perfil do sensor: o mesmo
// valor 1 significa ROTATING no acelerômetro e LEVEL no microfone.
typedef enum {
    ACOUSTIC_MODE_IDLE    = 0x00, // aquisição pausada
    ACOUSTIC_MODE_LEVEL   = 0x01, // nível sonoro, estatística e dose de ruído (NR-15 / NHO-01)
    ACOUSTIC_MODE_MACHINE = 0x02, // assinatura acústica: tons, harmônicos e anomalia
    ACOUSTIC_MODE_EVENTS  = 0x03  // sentinela de eventos sonoros: STA/LTA e classificação
} AcousticModeFSM;

typedef enum {
    ACQ_MODE_UNKNOWN   = 0x00,
    ACQ_MODE_DRDY      = 0x01, // INT/DRDY ativo, leitura I2C fora da ISR
    ACQ_MODE_POLLING   = 0x02, // fallback por polling I2C
    ACQ_MODE_SIMULATED = 0x03, // reservado; aquisição sintética não habilitada
    ACQ_MODE_IDLE      = 0x04, // aquisição pausada
    ACQ_MODE_I2S_DMA   = 0x05  // microfone I2S lido pela PIO com DMA contínuo (perfil acústico)
} AcquisitionMode;

#define BATTERY_PCT_UNKNOWN 0xFF
#define AXIS_MASK_X         0x01
#define AXIS_MASK_Y         0x02
#define AXIS_MASK_Z         0x04
#define AXIS_MASK_VECTOR    0x08

// ============================================================================
// ITEM 9: TAXONOMIA DE KERNEL - DIAGNOSTIC TROUBLE CODES (DTC / UDS / OBD-II)
// ============================================================================
typedef enum {
    DTC_CAT_BUS       = 0x1000, // Falhas de Barramento / Comunicação
    DTC_CAT_MECHANIC  = 0x2000, // Falhas Físicas e Mecânicas do Sensor
    DTC_CAT_DSP       = 0x3000, // Falhas de Processamento de Sinais / RAM
    DTC_CAT_SYSTEM    = 0x4000  // Falhas Gerais do RP2350 (Rede, Energia, WDT)
} DTC_Category;

typedef enum {
    DTC_NONE            = 0x0000,
    DTC_I2C_MPU_COMM    = DTC_CAT_BUS      | 0x01, // Falha de comunicação no I2C0
    DTC_I2C_BUS_STUCK   = DTC_CAT_BUS      | 0x02, // Linha SDA presa em GND / timeout de clock stretching
    DTC_I2S_NO_DATA     = DTC_CAT_BUS      | 0x03, // Microfone I2S sem dados válidos (SD parado em 0 ou em 1)
    DTC_SENS_GRAVITY    = DTC_CAT_MECHANIC | 0x01, // Norma de gravidade fora de 1g (Soltura)
    DTC_SENS_CLIPPING   = DTC_CAT_MECHANIC | 0x02, // Saturação mecânica do acelerômetro (+-2g nesta configuração)
    DTC_SENS_ACOUSTIC_OVERLOAD = DTC_CAT_MECHANIC | 0x03, // Pressão sonora acima do ponto de sobrecarga do microfone
    DTC_DSP_OVERRUN     = DTC_CAT_DSP      | 0x01, // Estouro de tempo no cálculo da FFT
    DTC_DSP_CORE_STALL  = DTC_CAT_DSP      | 0x02, // Núcleo dedicado ao DSP sem progresso (vigilância do núcleo 1)
    DTC_SYS_LOW_VOLTAGE = DTC_CAT_SYSTEM   | 0x01, // Tensão da bateria crítica
    DTC_SYS_NET_TX_FAIL = DTC_CAT_SYSTEM   | 0x02, // Falhas repetidas de envio UDP
    DTC_SYS_AUTH_REJECT       = DTC_CAT_SYSTEM | 0x03, // Credencial/chave rejeitada
    DTC_SYS_PROTOCOL_MISMATCH = DTC_CAT_SYSTEM | 0x04, // Versão incompatível
    DTC_SYS_REPLAY_REJECT     = DTC_CAT_SYSTEM | 0x05, // Contador de controle repetido/antigo
    DTC_SYS_CONFIG_REJECT     = DTC_CAT_SYSTEM | 0x06  // Payload de configuração inválido
} DTC_Code;

typedef enum {
    FTB_NO_SYMPTOM          = 0x00,
    FTB_GENERAL_FAILURE     = 0x01,
    FTB_SIGNAL_OPEN         = 0x04, // Circuito aberto / Sem sinal (WHO_AM_I falhou)
    FTB_SIGNAL_STUCK_LOW    = 0x11, // Linha em curto para GND
    FTB_SIGNAL_STUCK_HIGH   = 0x12, // Linha presa em nível alto
    FTB_OUT_OF_RANGE_HIGH   = 0x16, // Valor acima do limite máximo (Saturação / Choc)
    FTB_OUT_OF_RANGE_LOW    = 0x17, // Valor abaixo do limite (Gravidade < 0.8g ou VCC < 3.2V)
    FTB_SIGNAL_RATE_INVALID = 0x1F  // Taxa de amostragem corrompida (Overrun DSP)
} DTC_Symptom;

typedef enum {
    SEV_INFO     = 0, // Registro informativo
    SEV_WARNING  = 1, // Degradação operacional (Precisão reduzida)
    SEV_CRITICAL = 2  // Falha Fatal (O nó deve suspender o processamento ativo)
} DTC_Severity;

typedef struct __attribute__((packed)) {
    uint32_t timestamp_ms;      // Uptime no milissegundo da falha
    uint8_t  fsm_state;         // Modo operacional ativo
    int8_t   mpu_temp_c;        // Temperatura do die MPU-6050 (°C); INT8_MIN no sensor acústico (INMP441 não mede)
    int8_t   mcu_temp_c;        // Temperatura do die RP2350 (°C)
    uint16_t vcc_mv;            // Tensão do barramento de alimentação (mV)
} DTC_FreezeFrame;

typedef struct __attribute__((packed)) {
    uint16_t        dtc_code;   // Código da Falha
    uint8_t         symptom;    // Sintoma FTB
    uint8_t         severity;   // 0=Info, 1=Warning, 2=Critical
    DTC_FreezeFrame freeze;     // Snapshot de contexto no momento da falha
} DTC_Record;

// ============================================================================
// ESTRUTURAS DAS MENSAGENS UDP (ZERO-COPY)
// ============================================================================
typedef struct __attribute__((packed)) {
    uint16_t         magic_header;   // 0xAA55
    uint8_t          cmd_type;       // CMD_BEACON_BROADCAST (0x01)
    uint64_t         node_uuid;      // identificador derivado do flash ID
    uint8_t          profile_id;     // NodeProfileID (ex.: 0x01 = Acelerômetro)
    uint8_t          bite_status;    // 0x00 = Hardware OK; >0x00 = hint compacto (ver main.c)
    uint8_t          battery_pct;    // 0 a 100% ou BATTERY_PCT_UNKNOWN
    uint16_t         battery_mv;     // 0xFFFF se desconhecido
    uint8_t          acquisition_mode;
    uint8_t          dtc_count;
    uint8_t          protocol_ver;
} Payload_Beacon;

typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;       // CMD_CLAIM_NODE (0x10)
    uint64_t         session_token;
    uint8_t          protocol_ver;   // deve bater com NET_PROTOCOL_VERSION
    uint64_t         auth_key;       // sem uso na v0x06
} Payload_ClaimCommand;

typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;       // CMD_ACK_CAPABILITIES (0x11)
    uint64_t         session_token;
    uint64_t         node_uuid;
    uint8_t          profile_id;     // NodeProfileID
    uint32_t         max_sample_rate;
    uint16_t         dsp_buffer_max;
    uint8_t          dsp_fw_version;
} Payload_CapabilitiesAck;

typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;       // CMD_SET_CONFIG (0x12)
    uint64_t         session_token;
    uint32_t         request_counter; // estritamente crescente dentro da sessão
    uint8_t          target_mode;    // OperationModeFSM
    float            sample_rate_hz;
    uint16_t         window_size;    // 256 ou 512 pontos
    uint8_t          window_type;    // 0=Rect, 1=Hann, 2=Hamming, 3=FlatTop, 4=Blackman-Harris
    float            stalta_thresh;  // Limiar de alarme STA/LTA (ex: 4.0f)
    float            calib_gain;     // Ganho de calibração de fábrica
} Payload_Configuration;

// CMD_PING / CMD_PONG: liveness lease mínimo de sessão. O ESP32 deve mandar
// CMD_PING a cada NET_LIVENESS_INTERVAL_MS enquanto o sensor estiver
// NET_STATE_BOUND; o sensor responde com CMD_PONG e reinicia seu contador de
// timeout de sessão. Qualquer outro comando válido do nó CAN pai (ex.:
// CMD_SET_CONFIG) também conta como sinal de vida, então o ping só é
// necessário em períodos ociosos.
typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;       // CMD_PING (0x14) ou CMD_PONG (0x15)
    uint64_t         session_token;
    uint32_t         request_counter;
} Payload_Heartbeat;

// CMD_GET_CONFIG_MENU / CMD_CONFIG_MENU: descreve os valores válidos de
// configuração e o estado atual, para que o gateway (ou uma TUI via serial)
// consiga montar um menu SEM precisar hardcodar faixas/enums duplicados.
// Enviado automaticamente pelo nó logo após CMD_ACK_CAPABILITIES, e também
// sob demanda via CMD_GET_CONFIG_MENU a qualquer momento enquanto vinculado.
// O equivalente por serial é o comando texto "MENU" (ver README.md).
typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;             // CMD_CONFIG_MENU (0x17)
    uint64_t         session_token;
    uint8_t          num_fsm_modes;        // modos válidos: 0 .. num_fsm_modes-1
    uint8_t          num_window_types;     // janelas válidas: 0 .. num_window_types-1
    uint32_t         min_sample_rate_hz;
    uint32_t         max_sample_rate_hz;
    uint16_t         min_buffer_size;      // menor janela suportada
    uint16_t         max_buffer_size;      // maior janela suportada
    uint16_t         current_buffer_size;  // janela efetiva atual
    uint8_t          current_fsm_mode;     // OperationModeFSM
    uint8_t          current_window_type;  // WindowType
    float            current_sample_rate_hz;
    float            current_stalta_thresh;
    float            current_calib_gain;
} Payload_ConfigMenu;

typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;          // CMD_TELEMETRY_STREAM (0x20)
    uint64_t         session_token;
    uint16_t         seq_num;           // Contador para detectar perda de datagramas
    uint8_t          fsm_mode;          // OperationModeFSM
    uint8_t          acquisition_mode;  // AcquisitionMode
    uint8_t          axis_mask;         // AXIS_MASK_* usado no DSP
    uint8_t          fft_valid;         // 1 quando fft_array contém espectro válido
    uint16_t         window_size;       // janela efetivamente processada
    float            sample_rate_req_hz;// taxa solicitada
    float            sample_rate_eff_hz;// taxa efetiva do divisor do MPU
    float            rms_ac;
    float            kurtosis;
    float            crest_factor;
    float            peak_freq_hz;
    float            peak_amplitude;
    float            entropy;
    float            ppv_max_mm_s;
    uint8_t          seismic_triggered;
    uint8_t          clipping_detected;
    uint8_t          battery_pct;       // 0..100 ou BATTERY_PCT_UNKNOWN
    uint16_t         battery_mv;        // 0xFFFF se desconhecido
    uint8_t          dtc_count;
    uint16_t         dtc_active_code;
    uint16_t         fft_bins_count;
    // float         fft_array[];       // Vetor dinâmico opcional ao final do payload
} Payload_TelemetryStream;

typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;          // CMD_ACK_CONFIG
    uint64_t         session_token;
    uint32_t         request_counter;   // eco da requisição para correlação
    uint8_t          status;            // 0=queued, 1=applied, 2=rejected
    uint8_t          target_mode;
    uint16_t         requested_window_size;
    uint16_t         effective_window_size;
    float            requested_sample_rate_hz;
    float            effective_sample_rate_hz;
} Payload_ConfigAck;

typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;          // CMD_CLEAR_DTC
    uint64_t         session_token;
    uint32_t         request_counter;
} Payload_ClearDTC;

typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;          // CMD_DTC_SNAPSHOT
    uint64_t         session_token;
    uint8_t          dtc_count;
    uint16_t         active_code;
    uint16_t         auth_reject_count;
    uint16_t         protocol_reject_count;
    uint16_t         replay_reject_count;
    uint16_t         config_reject_count;
} Payload_DTCSnapshot;

// ============================================================================
// SENSOR ACÚSTICO (PROFILE_ACOUSTIC_MEMS, INMP441 em I2S) — v0.19
// ============================================================================
// O sensor acústico reaproveita todo o enlace (oferta BLE, HELLO/CHALLENGE/
// CONFIRM, DATA_UP/DATA_DOWN), Payload_Configuration, Payload_ConfigMenu,
// Payload_ConfigAck, CMD_PING e os DTCs. O que muda é o conteúdo da
// telemetria: um relatório por segundo (Payload_AcousticStream), com níveis
// já integrados no sensor, e dois comandos próprios (CMD_ACOUSTIC_CONTROL).
//
// Campos de Payload_Configuration no perfil acústico:
//   target_mode    AcousticModeFSM
//   sample_rate_hz 8000, 16000, 24000, 32000 ou 48000 Hz (frequência de WS)
//   window_size    pontos da FFT: 256, 512, 1024 ou 2048
//   window_type    mesmas janelas do acelerômetro
//   stalta_thresh  limiar do modo: razão STA/LTA em EVENTS, escore z em MACHINE
//   calib_gain     ganho de calibração da pressão (1,0 = sensibilidade nominal)

#define ACOUSTIC_OCTAVE_BANDS   10          // bandas de oitava de 31,5 Hz a 16 kHz (ISO 266)
#define ACOUSTIC_CB_NA          INT16_MIN   // nível indisponível, em centésimos de dB
#define ACOUSTIC_AGE_NONE       0xFFFFu     // nenhum evento desde o reinício da medição

// Bits de Payload_AcousticStream.flags
#define ACOUSTIC_FLAG_SPECTRUM_VALID    0x01 // ao menos um bloco do intervalo passou pela FFT
#define ACOUSTIC_FLAG_OVERLOAD          0x02 // amostras no fundo de escala (sobrecarga acústica)
#define ACOUSTIC_FLAG_EVENT_ACTIVE      0x04 // EVENTS: detector disparado neste instante
#define ACOUSTIC_FLAG_BASELINE_READY    0x08 // MACHINE: assinatura de referência disponível
#define ACOUSTIC_FLAG_BASELINE_LEARNING 0x10 // MACHINE: aprendizado da referência em curso
#define ACOUSTIC_FLAG_ANOMALY           0x20 // MACHINE: escore acima do limiar
#define ACOUSTIC_FLAG_DOSE_LIMIT        0x40 // LEVEL: dose >= 100 % ou nível acima de 115 dB(A)
#define ACOUSTIC_FLAG_MIC_FAULT         0x80 // microfone sem sinal válido

typedef enum {
    ACOUSTIC_EVENT_NONE      = 0,
    ACOUSTIC_EVENT_IMPACT    = 1, // curto e impulsivo, energia em médias e baixas frequências
    ACOUSTIC_EVENT_CRACK     = 2, // curto e impulsivo, agudo (estalo, quebra)
    ACOUSTIC_EVENT_TONAL     = 3, // longo e tonal (alarme, apito, sirene)
    ACOUSTIC_EVENT_TRANSIENT = 4  // longo e de banda larga (passagem, partida de máquina)
} AcousticEventClass;

typedef enum {
    ACOUSTIC_CTRL_MEASURE_RESET  = 1, // reinicia LAeq,T, estatísticas, doses e contagem de eventos
    ACOUSTIC_CTRL_BASELINE_LEARN = 2, // aprende a assinatura de referência por `argument` segundos
    ACOUSTIC_CTRL_BASELINE_CLEAR = 3  // descarta a assinatura de referência
} AcousticControlAction;

// Relatório acústico, um por segundo. Níveis em centésimos de dB re 20 µPa;
// ACOUSTIC_CB_NA marca o que o modo atual não calcula. Ao final seguem
// `spectrum_bins` magnitudes float: pressão eficaz (Pa) por faixa do espectro
// médio do intervalo (Welch), de 0 a fs/2.
typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;          // CMD_ACOUSTIC_STREAM (0x21)
    uint16_t         seq_num;
    uint8_t          mode;              // AcousticModeFSM
    uint8_t          flags;             // ACOUSTIC_FLAG_*
    uint8_t          window_type;
    uint16_t         fft_size;
    float            sample_rate_req_hz;
    float            sample_rate_eff_hz;
    uint16_t         interval_ms;       // duração integrada neste relatório
    uint8_t          coverage_pct;      // parcela do áudio analisada pela FFT
    uint8_t          cpu_pct;           // ocupação do núcleo de DSP
    // Níveis do intervalo
    int16_t          laeq_cb;
    int16_t          lceq_cb;
    int16_t          lzeq_cb;
    int16_t          lzpeak_cb;         // pico da pressão sem ponderação (dB re 20 µPa)
    int16_t          laf_cb;            // LAF (rápida, 125 ms) no fim do intervalo
    int16_t          lafmax_cb;
    int16_t          lafmin_cb;
    int16_t          crest_x100;        // fator de crista (pico / eficaz)
    int16_t          kurtosis_x100;     // excesso de curtose (0 = gaussiano)
    float            peak_hz;           // componente espectral dominante
    uint16_t         centroid_hz;       // centroide espectral
    uint16_t         flatness_x10000;   // planura espectral: 0 tonal, 1 ruído branco
    // LEVEL: desde o último reinício da medição
    int16_t          leq_total_cb;      // LAeq,T
    int16_t          l10_cb;
    int16_t          l50_cb;
    int16_t          l90_cb;
    uint16_t         dose_nr15_x10;     // dose em décimos de %: 85 dB(A), q = 5, limiar 80 dB(A)
    uint16_t         dose_nho01_x10;    // dose em décimos de %: 85 dB(A), q = 3, limiar 80 dB(A)
    uint32_t         elapsed_s;
    // MACHINE
    int16_t          tonal_cb;          // proeminência do componente dominante
    float            f0_hz;             // fundamental da série harmônica (0 = não encontrada)
    uint8_t          harmonics;         // harmônicos presentes na série
    int16_t          anomaly_x100;      // escore z em relação à assinatura de referência
    uint8_t          baseline_pct;      // progresso do aprendizado (100 = pronta)
    // EVENTS
    uint16_t         event_count;       // desde o último reinício da medição
    uint8_t          events_interval;   // eventos iniciados neste intervalo
    uint8_t          event_class;       // AcousticEventClass do último evento
    int16_t          event_peak_cb;
    int16_t          event_sel_cb;      // nível de exposição sonora do evento (SEL, ponderação A)
    uint16_t         event_duration_ms;
    uint16_t         event_centroid_hz;
    uint16_t         event_age_s;       // tempo desde o fim do último evento; ACOUSTIC_AGE_NONE
    int16_t          sta_lta_cb;        // razão STA/LTA atual, em dB
    int16_t          background_cb;     // ruído de fundo (LTA), dB(A)
    // Bandas de oitava sem ponderação (Z), 31,5 Hz .. 16 kHz
    int16_t          octave_cb[ACOUSTIC_OCTAVE_BANDS];
    // Sistema
    uint8_t          battery_pct;
    uint16_t         battery_mv;
    uint8_t          dtc_count;
    uint16_t         dtc_active_code;
    uint16_t         spectrum_bins;     // float spectrum[spectrum_bins] após a estrutura
} Payload_AcousticStream;

typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;          // CMD_ACOUSTIC_CONTROL (0x1C)
    uint32_t         request_counter;   // ecoado no ACK
    uint8_t          action;            // AcousticControlAction
    uint16_t         argument;          // BASELINE_LEARN: duração em segundos
} Payload_AcousticControl;

typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;          // CMD_ACOUSTIC_CONTROL_ACK (0x1D)
    uint32_t         request_counter;
    uint8_t          action;
    uint8_t          status;            // 1 = aplicado, 2 = recusado
    uint16_t         detail;
} Payload_AcousticControlAck;

#endif // EDGE_PROTOCOL_DEFINITIONS_H
