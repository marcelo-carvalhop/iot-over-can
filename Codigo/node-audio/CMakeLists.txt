cmake_minimum_required(VERSION 3.13)

# Sensor acústico INMP441 para Raspberry Pi Pico W (RP2040) e Pico 2 W (RP2350).
# A placa precisa ser definida ANTES de pico_sdk_init(): só assim o SDK
# configura o rádio CYW43439 (Wi-Fi/BLE) das duas placas.
#   cmake -DPICO_BOARD=pico_w  ..   (padrão)
#   cmake -DPICO_BOARD=pico2_w ..
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico_w CACHE STRING "Board type")
endif()

# O enlace e os módulos comuns aos sensores ficam no firmware de vibração.
set(EDGE_WIFI_DIR ${CMAKE_CURRENT_LIST_DIR}/../node-wifi)
set(IOC_LINK_DIR ${CMAKE_CURRENT_LIST_DIR}/../common/ioc_link)

include(${EDGE_WIFI_DIR}/pico_sdk_import.cmake)

project(edge_audio_node C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

if(NOT PICO_CYW43_SUPPORTED)
    message(FATAL_ERROR "O sensor acústico precisa do rádio CYW43: use PICO_BOARD=pico_w ou pico2_w (atual: ${PICO_BOARD})")
endif()

set(AUDIO_SOURCES
    main.c
    audio_app.c
    audio_config.c
    audio_console.c
    audio_core1.c
    inmp441_i2s.c
    acoustic_dsp.c
    audio_fft.c
)

add_executable(edge_audio_firmware
    ${AUDIO_SOURCES}
    # Comuns aos sensores (Codigo/node-wifi): enlace, BLE, identidade, bateria, DTCs.
    ${EDGE_WIFI_DIR}/edge_network_driver.c
    ${EDGE_WIFI_DIR}/edge_ble_beacon.c
    ${EDGE_WIFI_DIR}/edge_link.c
    ${EDGE_WIFI_DIR}/edge_diagnostics.c
    ${EDGE_WIFI_DIR}/device_identity.c
    ${EDGE_WIFI_DIR}/battery_monitor.c
    ${IOC_LINK_DIR}/ioc_sha256.c
    ${IOC_LINK_DIR}/ioc_link.c
)

# Programa da PIO (receptor I2S) convertido em cabeçalho C no build.
pico_generate_pio_header(edge_audio_firmware ${CMAKE_CURRENT_LIST_DIR}/inmp441_i2s.pio)

# lwipopts.h e btstack_config.h vêm de node-wifi: a mesma pilha de rede.
target_include_directories(edge_audio_firmware PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${EDGE_WIFI_DIR}
    ${IOC_LINK_DIR}
)

target_link_libraries(edge_audio_firmware
    pico_stdlib
    pico_multicore
    pico_util
    hardware_pio
    hardware_dma
    hardware_irq
    hardware_clocks
    hardware_gpio
    hardware_i2c
    hardware_adc
    hardware_timer
    hardware_watchdog
    hardware_sync
    pico_cyw43_arch_lwip_threadsafe_background
    pico_unique_id
    pico_rand
    pico_btstack_ble
    pico_btstack_cyw43
)

# Avisos tratados como erro no código deste diretório (o do SDK fica de fora).
set_source_files_properties(${AUDIO_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

pico_enable_stdio_usb(edge_audio_firmware 1)
pico_enable_stdio_uart(edge_audio_firmware 0)
pico_add_extra_outputs(edge_audio_firmware)

# Pinos do INMP441 (WS é sempre SCK + 1, exigência do side-set da PIO).
set(AUDIO_PIN_SCK 18 CACHE STRING "GPIO do SCK do INMP441 (WS = SCK + 1)")
set(AUDIO_PIN_SD 20 CACHE STRING "GPIO do SD do INMP441")

# Segredos fornecidos na configuração do build, nunca versionados (mesmo
# esquema do sensor de vibração; ver Codigo/scripts/build_pico.sh --audio).
set(EDGE_SERIAL_ADMIN_TOKEN "0" CACHE STRING "64-bit maintenance token; 0 keeps mutating serial commands locked")
set(EDGE_LINK_DEVICE_KEY "" CACHE STRING "K_dev (64 hex): chave do enlace derivada para este sensor")
set(EDGE_LINK_NETWORK_KEY "" CACHE STRING "K_net (64 hex): chave da rede Wi-Fi local dos Nodes")
set(EDGE_LINK_DEVICE_UUID "" CACHE STRING "UUID64 (16 hex) para o qual K_dev foi derivada; vazio não confere")
set(EDGE_LINK_MASTER_KEY "" CACHE STRING "K_master (64 hex): somente bancada, quando o UUID ainda não é conhecido")

target_compile_definitions(edge_audio_firmware PRIVATE
    EDGE_SERIAL_ADMIN_TOKEN=${EDGE_SERIAL_ADMIN_TOKEN}ULL
    EDGE_LINK_DEVICE_KEY_HEX="${EDGE_LINK_DEVICE_KEY}"
    EDGE_LINK_NETWORK_KEY_HEX="${EDGE_LINK_NETWORK_KEY}"
    EDGE_LINK_DEVICE_UUID_HEX="${EDGE_LINK_DEVICE_UUID}"
    EDGE_LINK_MASTER_KEY_HEX="${EDGE_LINK_MASTER_KEY}"
    EDGE_BOARD_NAME="${PICO_BOARD}"
    AUDIO_PIN_SCK=${AUDIO_PIN_SCK}
    AUDIO_PIN_SD=${AUDIO_PIN_SD}
)
