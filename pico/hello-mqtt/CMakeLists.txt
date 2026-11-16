# hello-mqtt: Pico 2 W (or Pico W) to AWS IoT Core over MQTT, with a key made on the Pico.
#
#   export PICO_SDK_PATH=/path/to/pico-sdk                    # 2.1 or later
#   cmake -S pico/hello-mqtt -B build/pico -DPICO_BOARD=pico2_w
#   cmake --build build/pico                                  # -> build/pico/hello_mqtt.uf2
#
# The firmware holds no Wi-Fi settings or other secrets: pico/tools/pico-enrol.py sends them over USB.
#
# For a Pico 2 W the key is wrapped by the key ladder (HELLO_KEY_LADDER, below). PICO_BOARD=pico_w builds for
# the Pico W (RP2040), whose key is plain in flash: fine for trying it out, but see the README on its key.
cmake_minimum_required(VERSION 3.13...3.31)

set(PICO_BOARD pico2_w CACHE STRING "pico2_w or pico_w")
if(NOT DEFINED PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH to a Pico SDK 2.1 or later checkout")
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(hello_mqtt C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()
if(PICO_SDK_VERSION_STRING VERSION_LESS "2.1.0")
    message(FATAL_ERROR "Pico SDK 2.1.0 or later required")
endif()

if(DEFINED WIFI_SSID OR DEFINED WIFI_PASSWORD)
    message(FATAL_ERROR "WIFI_SSID and WIFI_PASSWORD are no longer built in: send them with pico-enrol.py wifi. "
                        "Remove them from the CMake cache (a new build directory, or -UWIFI_SSID -UWIFI_PASSWORD).")
endif()

# coreMQTT (MIT), the AWS reference MQTT client, pinned to the commit of tag v2.3.1 (a tag can be moved, a
# commit cannot). Only its sources are used.
include(FetchContent)
FetchContent_Declare(coremqtt
    GIT_REPOSITORY https://github.com/FreeRTOS/coreMQTT
    GIT_TAG 2beef04725328923e05e576b884212d53ec97af7  # v2.3.1
    SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(coremqtt)
include(${coremqtt_SOURCE_DIR}/mqttFilePaths.cmake)
add_library(coremqtt STATIC ${MQTT_SOURCES} ${MQTT_SERIALIZER_SOURCES})
target_include_directories(coremqtt PUBLIC ${MQTT_INCLUDE_PUBLIC_DIRS} ${CMAKE_CURRENT_LIST_DIR}/config)

# Amazon Root CA 1, which AWS IoT's endpoints chain to: downloaded at build time and checked against its
# SHA-256 (certificate fingerprint 8E:CD:E6:88:...:19:6E), then made into a C header.
set(AWS_ROOT_CA ${CMAKE_CURRENT_BINARY_DIR}/AmazonRootCA1.pem)
file(DOWNLOAD https://www.amazontrust.com/repository/AmazonRootCA1.pem ${AWS_ROOT_CA}
     EXPECTED_HASH SHA256=2c43952ee9e000ff2acc4e2ed0897c0a72ad5fa72c3d934e81741cbd54f05bd1)
file(STRINGS ${AWS_ROOT_CA} ca_lines)
set(ca_c "")
foreach(l IN LISTS ca_lines)
    string(APPEND ca_c "    \"${l}\\n\"\n")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/aws_root_ca.h
     "/* Generated from AmazonRootCA1.pem by CMakeLists.txt. */\nstatic const char AWS_ROOT_CA_PEM[] =\n${ca_c};\n")

add_executable(hello_mqtt main.c identity.c mqtt.c)
target_include_directories(hello_mqtt PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(hello_mqtt PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/config)
target_compile_definitions(hello_mqtt PRIVATE
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000
    # Lines end in "\n" only: the SDK's "\n" to "\r\n" translation lost line ends under load in testing.
    PICO_STDIO_ENABLE_CRLF_SUPPORT=0)
target_compile_options(hello_mqtt PRIVATE -Wall -Wextra)
target_link_libraries(hello_mqtt
    coremqtt
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mbedtls
    pico_lwip_sntp
    pico_mbedtls_headers
    pico_mbedtls_crypto
    pico_mbedtls_x509
    pico_mbedtls_tls
    pico_unique_id
    pico_flash
    hardware_flash)

# The key ladder (ladder.h): the key wrapped under a root key in OTP. The firmware never writes OTP by itself;
# pico/tools/pico-otp-init.py writes the root, after a dry run and your confirmation. Pico 2 W only, and on by
# default there; -DHELLO_KEY_LADDER=OFF keeps the key plain in flash (docs/key-storage.md).
if(PICO_PLATFORM MATCHES "rp2350")
    set(key_ladder_default ON)
else()
    set(key_ladder_default OFF)
endif()
option(HELLO_KEY_LADDER "Wrap the key under a root key in OTP (Pico 2 W)" ${key_ladder_default})
set(OTP_ROOT_PAGE 60 CACHE STRING "OTP page (4-60) for the key ladder's root")
if(HELLO_KEY_LADDER)
    if(NOT PICO_PLATFORM MATCHES "rp2350")
        message(FATAL_ERROR "HELLO_KEY_LADDER needs a Pico 2 W (RP2350): the RP2040 has no OTP for it")
    endif()
    target_sources(hello_mqtt PRIVATE ladder.c)
endif()
target_compile_definitions(hello_mqtt PRIVATE HELLO_KEY_LADDER=$<BOOL:${HELLO_KEY_LADDER}> OTP_ROOT_PAGE=${OTP_ROOT_PAGE})

if(PICO_PLATFORM MATCHES "rp2350")
    # RP2350: keys come from the TRNG, with its health tests on (trng_rp2350.c). pico_rand must then leave
    # the TRNG alone (it bypasses the health tests), so it is built from its other sources.
    target_sources(hello_mqtt PRIVATE trng_rp2350.c)
    target_compile_definitions(hello_mqtt PRIVATE PICO_RAND_ENTROPY_SRC_TRNG=0 PICO_RAND_ENTROPY_SRC_BUS_PERF_COUNTER=1)
else()
    # RP2040 has no TRNG: mbedTLS takes its entropy from pico_rand, which is not a cryptographic generator.
    # Good enough to try things out; not for products (see the README).
    target_link_libraries(hello_mqtt pico_rand pico_mbedtls)
endif()

pico_enable_stdio_usb(hello_mqtt 1)
pico_enable_stdio_uart(hello_mqtt 0)
pico_add_extra_outputs(hello_mqtt)
