cmake_minimum_required(VERSION 3.13)

# Item 17: define a placa ANTES de pico_sdk_init(). Sem isso o SDK não
# necessariamente configura os pinos/firmware do rádio CYW43439 do Pico W
# corretamente — pode compilar "silenciosamente" para a placa errada.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico_w CACHE STRING "Board type")
endif()

# Inicializa o Pico SDK
include(pico_sdk_import.cmake)

project(edge_dsp_vibration_node C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Inicializa o Raspberry Pi Pico SDK
pico_sdk_init()

# Biblioteca compartilhada com os Nodes CAN: HMAC-SHA256 e autenticação do enlace.
set(IOC_LINK_DIR ${CMAKE_CURRENT_LIST_DIR}/../common/ioc_link)

# Define o alvo como Raspberry Pi Pico W (RP2040 com rádio CYW43439)
add_executable(edge_node_firmware
    main.c
    dsp_pipeline.c
    mpu6050_dma_driver.c
    edge_network_driver.c
    serial_console.c
    battery_monitor.c
    config_validation.c
    device_identity.c
    edge_ble_beacon.c
    edge_link.c
    ${IOC_LINK_DIR}/ioc_sha256.c
    ${IOC_LINK_DIR}/ioc_link.c
)

# Adiciona diretórios de cabeçalho (inclui também lwipopts.h, que precisa
# estar visível no include path do pico_cyw43_arch_lwip_*)
target_include_directories(edge_node_firmware PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${IOC_LINK_DIR})

# Linka as bibliotecas nativas de hardware, I2C, Wi-Fi e lwIP do Pico SDK
target_link_libraries(edge_node_firmware
    pico_stdlib
    hardware_i2c
    hardware_gpio
    hardware_timer
    hardware_irq
    hardware_watchdog
    hardware_sync
    hardware_adc
    pico_cyw43_arch_lwip_threadsafe_background
    pico_unique_id
    pico_rand
    pico_btstack_ble
    pico_btstack_cyw43
)

# Habilita saída de debug via USB Serial (opcional, pode ser desativado para produção)
pico_enable_stdio_usb(edge_node_firmware 1)
pico_enable_stdio_uart(edge_node_firmware 0)

# Gera os arquivos .uf2, .elf, .hex e .bin para gravação no chip
pico_add_extra_outputs(edge_node_firmware)


# Secrets are supplied at configure time and are never committed to source.
# Codigo/scripts/build_pico.sh lê os valores de .env.local. Exemplo manual:
# cmake -DPICO_BOARD=pico_w -DEDGE_SERIAL_ADMIN_TOKEN=0x1122334455667788 \
#       -DEDGE_LINK_DEVICE_KEY=<64 hex> -DEDGE_LINK_NETWORK_KEY=<64 hex> \
#       -DEDGE_LINK_DEVICE_UUID=E6616408432B6F39 ..
set(EDGE_SERIAL_ADMIN_TOKEN "0" CACHE STRING "64-bit maintenance token; 0 keeps mutating serial commands locked")
set(EDGE_LINK_DEVICE_KEY "" CACHE STRING "K_dev (64 hex): chave do enlace derivada para este sensor")
set(EDGE_LINK_NETWORK_KEY "" CACHE STRING "K_net (64 hex): chave da rede Wi-Fi local dos Nodes")
set(EDGE_LINK_DEVICE_UUID "" CACHE STRING "UUID64 (16 hex) para o qual K_dev foi derivada; vazio não confere")
set(EDGE_LINK_MASTER_KEY "" CACHE STRING "K_master (64 hex): somente bancada, quando o UUID ainda não é conhecido")

target_compile_definitions(edge_node_firmware PRIVATE
    EDGE_SERIAL_ADMIN_TOKEN=${EDGE_SERIAL_ADMIN_TOKEN}ULL
    EDGE_LINK_DEVICE_KEY_HEX="${EDGE_LINK_DEVICE_KEY}"
    EDGE_LINK_NETWORK_KEY_HEX="${EDGE_LINK_NETWORK_KEY}"
    EDGE_LINK_DEVICE_UUID_HEX="${EDGE_LINK_DEVICE_UUID}"
    EDGE_LINK_MASTER_KEY_HEX="${EDGE_LINK_MASTER_KEY}"
)
