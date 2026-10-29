idf_component_register(
    SRCS
        "app_main.cpp"
        "lamp_driver.cpp"
        "knob_driver.cpp"
    INCLUDE_DIRS
        "."
        "include"
    REQUIRES
        esp_matter
        nvs_flash
        driver
        esp_driver_gpio
        esp_driver_ledc
        app_update
        esp_http_server
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
