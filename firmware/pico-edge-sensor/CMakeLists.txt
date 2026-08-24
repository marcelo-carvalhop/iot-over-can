cmake_minimum_required(VERSION 3.13)

# Item 17: define a placa ANTES de pico_sdk_init(). Sem isso o SDK não
# necessariamente configura os pinos/firmware do rádio CYW43439 do Pico 2 W
# corretamente — pode compilar "silenciosamente" para a placa errada.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w CACHE STRING "Board type")
endif()

# Inicializa o Pico SDK
include(pico_sdk_import.cmake)

project(edge_dsp_vibration_node C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Inicializa o Raspberry Pi Pico SDK
pico_sdk_init()

# Define o alvo como Raspberry Pi Pico 2W (RP2350 com Rádio CYW43439)
add_executable(edge_node_firmware
    main.c
    dsp_pipeline.c
    mpu6050_dma_driver.c
    edge_network_driver.c
    serial_console.c
    battery_monitor.c
)

# Adiciona diretórios de cabeçalho (inclui também lwipopts.h, que precisa
# estar visível no include path do pico_cyw43_arch_lwip_*)
target_include_directories(edge_node_firmware PRIVATE ${CMAKE_CURRENT_LIST_DIR})

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
)

# Habilita saída de debug via USB Serial (opcional, pode ser desativado para produção)
pico_enable_stdio_usb(edge_node_firmware 1)
pico_enable_stdio_uart(edge_node_firmware 0)

# Gera os arquivos .uf2, .elf, .hex e .bin para gravação no chip
pico_add_extra_outputs(edge_node_firmware)
