/**
 * @file edge_protocol_definitions.h
 * @brief Definições de estruturas binárias, protocolos de rede UDP e códigos DTC industriais.
 * Atende aos Itens 9 (Anatomia do DTC) e 12 (Arquitetura de Rede Híbrida).
 *
 * NOTA DE VERSIONAMENTO DE PROTOCOLO (v0x02):
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
 * - Novo par CMD_PING / CMD_PONG para heartbeat de sessão.
 * - Novo par CMD_GET_CONFIG_MENU / CMD_CONFIG_MENU: o nó anuncia os valores
 *   válidos de configuração (modos FSM, janelas, faixa de sample rate) e o
 *   estado atual, tanto no Wi-Fi quanto (em formato texto/JSON equivalente)
 *   na porta serial. Ver README.md, seção "Menu de Configuração".
 * Por isso NET_PROTOCOL_VERSION subiu de 0x02 para 0x03.
 */
#ifndef EDGE_PROTOCOL_DEFINITIONS_H
#define EDGE_PROTOCOL_DEFINITIONS_H

#include <stdint.h>
#include <stdbool.h>

#define NET_UDP_PORT_DISCOVERY  4242
#define NET_UDP_PORT_TELEMETRY  4243
#define NET_MAGIC_HEADER        0xAA55
#define NET_PROTOCOL_VERSION    0x04 // o nó rejeita CMD_CLAIM_NODE com protocol_ver diferente

#define MY_NODE_UUID            0x10A4  // ID único na rede (derivar do MAC na prática)

// Chave compartilhada mínima para impedir que QUALQUER host na sub-rede
// reivindique o nó por acidente (ex.: outro dispositivo testando o
// protocolo, um scanner de rede, etc.). NÃO é criptografia forte — é um
// "não seja o alvo mais fácil da rede". Para redes industriais isoladas
// (Soft-AP dedicado, como este projeto já usa) isso é proporcional ao
// risco; se o nó puder estar em uma rede compartilhada/hostil no futuro,
// troque por um handshake com desafio-resposta (HMAC) usando uma chave
// gravada por dispositivo (não uma constante de firmware).
#define NODE_PRESHARED_KEY      0x5A3C9E10u

// Tamanho máximo recomendado de payload UDP para evitar fragmentação IP em
// Wi-Fi (MTU típico 1500 - cabeçalhos IP/UDP ~28 bytes = ~1472 bytes úteis).
// edge_net_send_telemetry() usa isso para limitar quantos bins de FFT são
// anexados por pacote.
#define NET_MAX_SAFE_PAYLOAD_BYTES 1472

// Timeout de sessão: se o gateway não enviar nenhum comando válido
// (CMD_SET_CONFIG ou CMD_PING) dentro desse intervalo, o nó assume que o
// gateway caiu/reiniciou e volta para NET_STATE_DISCOVERY.
#define NET_SESSION_TIMEOUT_MS   10000
// Intervalo alvo em que o gateway deveria mandar CMD_PING (documentação
// para quem for implementar o lado ESP32).
#define NET_HEARTBEAT_INTERVAL_MS 3000

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
    CMD_BEACON_BROADCAST    = 0x01, // RP2350 -> Todos: "Estou disponível nesta sub-rede"
    CMD_CLAIM_NODE          = 0x10, // ESP32 -> RP2350: "Conecte-se a mim (Token gerado)"
    CMD_ACK_CAPABILITIES    = 0x11, // RP2350 -> ESP32: "Vínculo aceito. Estrutura anexa"
    CMD_SET_CONFIG          = 0x12, // ESP32 -> RP2350: "Atualize os parâmetros operacionais DSP"
    CMD_ACK_CONFIG          = 0x13, // RP2350 -> ESP32: "Configuração DSP aplicada com sucesso"
    CMD_PING                = 0x14, // ESP32 -> RP2350: "Heartbeat, sessão continua viva"
    CMD_PONG                = 0x15, // RP2350 -> ESP32: "Confirmação de heartbeat"
    CMD_GET_CONFIG_MENU     = 0x16, // ESP32 -> RP2350: "Me diga as opções/faixas de config válidas"
    CMD_CONFIG_MENU         = 0x17, // RP2350 -> ESP32: "Aqui estão os valores válidos + config atual"
                                     // (enviado automaticamente logo após CMD_ACK_CAPABILITIES)
    CMD_CLEAR_DTC           = 0x18, // ESP32 -> RP2350: limpa DTCs ativos sob sessão válida
    CMD_DTC_SNAPSHOT        = 0x19, // RP2350 -> ESP32: snapshot compacto da tabela de DTC
    CMD_TELEMETRY_STREAM    = 0x20, // RP2350 -> ESP32: "Pacote de telemetria contínua / FFT"
    CMD_URGENT_DTC_ALARM    = 0xFF  // RP2350 -> ESP32: "Alerta crítico BITE / Falha de hardware"
} NetworkCommandID;

typedef enum {
    FSM_MODE_IDLE           = 0x00, // Baixo consumo, aquisição pausada
    FSM_MODE_ROTATING_MACH  = 0x01, // Máquinas rolantes: RMS AC, Curtose, FFT Pico
    FSM_MODE_STRUCTURAL     = 0x02, // Análise estrutural: Fator de Cresta, Entropia Espectral
    FSM_MODE_SEISMIC_STALTA = 0x03  // Sismografia: Oversampling 18-bit, Trigger STA/LTA
} OperationModeFSM;

typedef enum {
    ACQ_MODE_UNKNOWN   = 0x00,
    ACQ_MODE_DRDY      = 0x01, // INT/DRDY ativo, leitura I2C fora da ISR
    ACQ_MODE_POLLING   = 0x02, // fallback por polling I2C
    ACQ_MODE_SIMULATED = 0x03, // buffer sintético de bancada
    ACQ_MODE_IDLE      = 0x04  // aquisição pausada
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
    DTC_SENS_GRAVITY    = DTC_CAT_MECHANIC | 0x01, // Norma de gravidade fora de 1g (Soltura)
    DTC_SENS_CLIPPING   = DTC_CAT_MECHANIC | 0x02, // Saturação mecânica do acelerômetro (+-2g nesta configuração)
    DTC_DSP_OVERRUN     = DTC_CAT_DSP      | 0x01, // Estouro de tempo no cálculo da FFT
    DTC_SYS_LOW_VOLTAGE = DTC_CAT_SYSTEM   | 0x01, // Tensão da bateria crítica
    DTC_SYS_NET_TX_FAIL = DTC_CAT_SYSTEM   | 0x02, // Falhas repetidas de envio UDP
    DTC_SYS_AUTH_REJECT = DTC_CAT_SYSTEM   | 0x03  // Tentativa de vínculo rejeitada (versão/chave)
} DTC_Code;

typedef enum {
    FTB_NO_SYMPTOM          = 0x00,
    FTB_GENERAL_FAILURE     = 0x01,
    FTB_SIGNAL_OPEN         = 0x04, // Circuito aberto / Sem sinal (WHO_AM_I falhou)
    FTB_SIGNAL_STUCK_LOW    = 0x11, // Linha em curto para GND
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
    int8_t   mpu_temp_c;        // Temperatura do die MPU-6050 (°C)
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
    uint16_t         node_uuid;      // 0x10A4
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
    uint16_t         session_token;
    uint8_t          protocol_ver;   // deve bater com NET_PROTOCOL_VERSION
    uint32_t         auth_key;       // deve bater com NODE_PRESHARED_KEY
} Payload_ClaimCommand;

typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;       // CMD_ACK_CAPABILITIES (0x11)
    uint16_t         session_token;
    uint16_t         node_uuid;
    uint8_t          profile_id;     // NodeProfileID
    uint32_t         max_sample_rate;
    uint16_t         dsp_buffer_max;
    uint8_t          dsp_fw_version;
} Payload_CapabilitiesAck;

typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;       // CMD_SET_CONFIG (0x12)
    uint16_t         session_token;
    uint8_t          target_mode;    // OperationModeFSM
    float            sample_rate_hz;
    uint16_t         window_size;    // 256 ou 512 pontos
    uint8_t          window_type;    // 0=Rect, 1=Hann, 2=Hamming, 3=FlatTop, 4=Blackman-Harris
    float            stalta_thresh;  // Limiar de alarme STA/LTA (ex: 4.0f)
    float            calib_gain;     // Ganho de calibração de fábrica
} Payload_Configuration;

// CMD_PING / CMD_PONG: heartbeat mínimo de sessão. O ESP32 deve mandar
// CMD_PING a cada NET_HEARTBEAT_INTERVAL_MS enquanto o nó estiver
// NET_STATE_BOUND; o nó responde com CMD_PONG e reinicia seu contador de
// timeout de sessão. Qualquer outro comando válido do gateway (ex.:
// CMD_SET_CONFIG) também conta como sinal de vida, então o ping só é
// necessário em períodos ociosos.
typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;       // CMD_PING (0x14) ou CMD_PONG (0x15)
    uint16_t         session_token;
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
    uint16_t         session_token;
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
    uint16_t         session_token;
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
    uint16_t         session_token;
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
    uint16_t         session_token;
} Payload_ClearDTC;

typedef struct __attribute__((packed)) {
    uint16_t         magic_header;
    uint8_t          cmd_type;          // CMD_DTC_SNAPSHOT
    uint16_t         session_token;
    uint8_t          dtc_count;
    uint16_t         active_code;
} Payload_DTCSnapshot;

#endif // EDGE_PROTOCOL_DEFINITIONS_H
