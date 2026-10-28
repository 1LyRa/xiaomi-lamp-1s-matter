cmake_minimum_required(VERSION 3.16)

# Globally disable precompiled headers to avoid header-order / macro conflicts
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "ESP_MATTER_PATH is not set")
endif()

if(NOT DEFINED ENV{ESP_MATTER_DEVICE_PATH})
    set(ENV{ESP_MATTER_DEVICE_PATH} $ENV{ESP_MATTER_PATH}/device_hal/device/esp32_devkit_c)
endif()

set(PROJECT_VER "1.0.0")
set(PROJECT_VER_NUMBER 1)

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
include($ENV{ESP_MATTER_DEVICE_PATH}/esp_matter_device.cmake)

set(EXTRA_COMPONENT_DIRS "${ESP_MATTER_PATH}/examples/common"
                         "${MATTER_SDK_PATH}/config/esp32/components"
                         "${ESP_MATTER_PATH}/components"
                         "${ESP_MATTER_PATH}/device_hal/device"
                         ${extra_components_dirs_append})

project(xiaomi_lamp_1s_matter)

# Ensure CHIP_HAVE_CONFIG_H is defined globally across all components
add_compile_definitions(CHIP_HAVE_CONFIG_H)
idf_build_set_property(COMPILE_DEFINITIONS "CHIP_HAVE_CONFIG_H" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-DCHIP_HAVE_CONFIG_H" APPEND)
