#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the LEDC PWM channels and power rail for the lamp.
 */
esp_err_t lamp_driver_init(void);

/**
 * @brief Turn the lamp ON or OFF.
 */
esp_err_t lamp_set_power(bool on);

/**
 * @brief Set the brightness level (1 to 254).
 */
esp_err_t lamp_set_brightness(uint8_t level);

/**
 * @brief Set the color temperature in mireds (153 to 370).
 */
esp_err_t lamp_set_color_temp(uint16_t mireds);

/**
 * @brief Query current lamp state.
 */
bool lamp_get_power(void);
uint8_t lamp_get_brightness(void);
uint16_t lamp_get_color_temp(void);

#ifdef __cplusplus
}
#endif
