cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.0.0")
set(PROJECT_VER_NUMBER 1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include($ENV{ESP_MATTER_PATH}/cmake/esp_matter_component.cmake)

project(xiaomi_lamp_1s_matter)
