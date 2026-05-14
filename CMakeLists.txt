# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if (WIN32)
    set(USERHOME $ENV{USERPROFILE})
else ()
    set(USERHOME $ENV{HOME})
endif ()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif ()
# ====================================================================================

if (PICO_W_BUILD)
        set(PICO_BOARD pico_w CACHE STRING "Board type")
        set(PICO_RP2350 OFF CACHE BOOL "Not using RP2350")
else ()
        set(PICO_BOARD pico2_w CACHE STRING "Board type")
endif ()
#set(PICO_CYW43_SUPPORTED 1)
#set(PICO_CYW43_ARCH_THREADSAFE_BACKGROUND 1)

option(ENABLE_SERIAL "Enable USB CDC serial printf output" OFF)
if (ENABLE_SERIAL)
        set(ENABLE_SERIAL_VALUE 1)
else ()
        set(ENABLE_SERIAL_VALUE 0)
endif ()
option(ENABLE_OTA_FIRMWARE "Enable browser Web Serial UF2 firmware updater over USB CDC" OFF)
if (ENABLE_OTA_FIRMWARE)
        set(ENABLE_OTA_FIRMWARE_VALUE 1)
else ()
        set(ENABLE_OTA_FIRMWARE_VALUE 0)
endif ()
option(ENABLE_OTA_AB_PARTITION "Enable RP2350 A/B partitioned TBYB update image" OFF)
if (ENABLE_OTA_AB_PARTITION)
        set(ENABLE_OTA_AB_PARTITION_VALUE 1)
else ()
        set(ENABLE_OTA_AB_PARTITION_VALUE 0)
endif ()
option(ENABLE_OTA_TBYB "Mark RP2350 OTA image as Try-Before-You-Buy" OFF)
if (ENABLE_OTA_TBYB)
        set(ENABLE_OTA_TBYB_VALUE 1)
else ()
        set(ENABLE_OTA_TBYB_VALUE 0)
endif ()
option(ENABLE_WATCHDOG "Enable runtime watchdog reset" OFF)
if (ENABLE_WATCHDOG)
        set(ENABLE_WATCHDOG_VALUE 1)
else ()
        set(ENABLE_WATCHDOG_VALUE 0)
endif ()
option(USB_CONNECT_BEFORE_BT "Enumerate USB before the controller handshake completes" OFF)
if (USB_CONNECT_BEFORE_BT)
        set(USB_CONNECT_BEFORE_BT_VALUE 1)
else ()
        set(USB_CONNECT_BEFORE_BT_VALUE 0)
endif ()
option(ENABLE_WIFI_FW_PARTITION "Place CYW43 firmware in a dedicated RP2350 partition" OFF)
option(ENABLE_VERBOSE "Enable Log Verbose" OFF)
if(ENABLE_VERBOSE)
    set(ENABLE_VERBOSE_VALUE 1)
else()
    set(ENABLE_VERBOSE_VALUE 0)
endif ()
option(ENABLE_BATT_LED "Blink Pico LED when DualSense battery is low (<=10%)" ON)
if (ENABLE_BATT_LED)
        set(ENABLE_BATT_LED_VALUE 1)
else ()
        set(ENABLE_BATT_LED_VALUE 0)
endif ()
option(DISABLE_SPEAKER_PROC "Disable Speaker in controller" OFF)
if (DISABLE_SPEAKER_PROC)
        set(DISABLE_SPEAKER_PROC_VALUE 1)
else ()
        set(DISABLE_SPEAKER_PROC_VALUE 0)
endif ()
option(PICO_W_BUILD "Build for Rasberry Pi Pico W, audio is missing" OFF)
if (PICO_W_BUILD)
        set(CYW43_PIO_CLOCK_DIV_VALUE 3)
        set(SYS_CLOCK_KHZ_VALUE 200000)
        set(DISABLE_SPEAKER_PROC_VALUE 1)
else ()
        set(CYW43_PIO_CLOCK_DIV_VALUE 5)
        set(SYS_CLOCK_KHZ_VALUE 320000)
endif ()



# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(ds5-bridge C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(ds5-bridge
        src/usb_descriptors.cpp
        src/main.cpp
        src/usb.cpp
        src/audio.cpp
        src/bt.cpp
        src/config.cpp
        src/cmd.cpp
        src/ota_firmware/ota_firmware.cpp
)

if (ENABLE_BATT_LED)
        target_sources(ds5-bridge PRIVATE src/battery_led.cpp)
endif ()

# Cockos WDLResampler
add_library(wdl_resampler STATIC
        lib/WDL/WDL/resample.cpp
)
target_include_directories(wdl_resampler PUBLIC
        lib/WDL/WDL
)

# Opus
#set(OPUS_FIXED_POINT ON)
add_subdirectory(lib/opus)

target_compile_options(wdl_resampler PRIVATE -fsigned-char)
target_compile_options(ds5-bridge PRIVATE -fsigned-char)
target_compile_definitions(wdl_resampler PRIVATE
        WDL_RESAMPLE_TYPE=float
)
target_compile_definitions(ds5-bridge PRIVATE
        CYW43_LWIP=0
        ENABLE_SERIAL=${ENABLE_SERIAL_VALUE}
        PICO_FLASH_ASSUME_CORE1_SAFE=1
        WDL_RESAMPLE_TYPE=float
        CYW43_PIO_CLOCK_DIV_INT=${CYW43_PIO_CLOCK_DIV_VALUE}
        PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
        PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
        PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
        ENABLE_OTA_FIRMWARE=${ENABLE_OTA_FIRMWARE_VALUE}
        ENABLE_OTA_AB_PARTITION=${ENABLE_OTA_AB_PARTITION_VALUE}
        ENABLE_VERBOSE=${ENABLE_VERBOSE_VALUE}
        ENABLE_BATT_LED=${ENABLE_BATT_LED_VALUE}
        SYS_CLOCK_KHZ=${SYS_CLOCK_KHZ_VALUE}
        DISABLE_SPEAKER_PROC=${DISABLE_SPEAKER_PROC_VALUE}
        ENABLE_WATCHDOG=${ENABLE_WATCHDOG_VALUE}
        USB_CONNECT_BEFORE_BT=${USB_CONNECT_BEFORE_BT_VALUE}
)

if (ENABLE_OTA_FIRMWARE AND ENABLE_OTA_TBYB AND NOT PICO_W_BUILD)
        target_compile_definitions(ds5-bridge PRIVATE
                PICO_CRT0_IMAGE_TYPE_TBYB=1
        )
        if (ENABLE_WIFI_FW_PARTITION)
                pico_use_wifi_firmware_partition(ds5-bridge)
        endif ()
        pico_hash_binary(ds5-bridge)
elseif (ENABLE_OTA_FIRMWARE AND NOT PICO_W_BUILD)
        # 非 TBYB 调试/首刷镜像仍保留 RP2350 hash，但不改变 IMAGE_DEF 类型，
        # 尽量接近未开启 OTA A/B 时的普通可启动镜像。
        pico_hash_binary(ds5-bridge)
endif ()

if (NOT PICO_W_BUILD)
        pico_set_uf2_family(ds5-bridge "rp2350-arm-s")
endif ()

option(ENABLE_WAKE_HID "Wake the Windows host on PS-button press" OFF)
if(ENABLE_WAKE_HID)
    target_compile_definitions(ds5-bridge PRIVATE
            ENABLE_WAKE_HID
    )
    target_sources(ds5-bridge PRIVATE
            src/wake.cpp
    )
endif()

option(WAKE_DEBUG "Print wake-FSM transitions to UART (developer use)" OFF)
if(WAKE_DEBUG)
    target_compile_definitions(ds5-bridge PRIVATE
            WAKE_DEBUG
    )
endif()
set(VERSION "dev" CACHE STRING "Program version string")

pico_set_program_name(ds5-bridge "ds5-bridge")
pico_set_program_version(ds5-bridge "${VERSION}")

# Modify the below lines to enable/disable output over UART/USB
# This target provides its own TinyUSB descriptors/device stack, so do not enable
# pico_stdio_usb here. Enabling pico_stdio_usb together with tinyusb_device can
# install a second CDC descriptor/polling path and cause Windows Code 43.
pico_enable_stdio_uart(ds5-bridge 1)
pico_enable_stdio_usb(ds5-bridge 0)

# Add the standard include files to the build
#target_include_directories(ds5-bridge PRIVATE
#        ${CMAKE_CURRENT_LIST_DIR}
#)

target_include_directories(${PROJECT_NAME} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add any user requested libraries
target_link_libraries(ds5-bridge
        pico_stdlib
        pico_multicore
        hardware_watchdog
        hardware_timer
        hardware_flash
        pico_sha256
        boot_uf2_headers
        pico_btstack_classic
#        pico_cyw43_arch_threadsafe_background
        pico_cyw43_arch_poll
        pico_btstack_cyw43
        tinyusb_device
        tinyusb_board
        wdl_resampler
        opus
)

pico_add_extra_outputs(ds5-bridge)

