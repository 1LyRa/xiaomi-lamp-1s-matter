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
 * @brief Visual status indicator states for Matter commissioning and factory reset.
 */
typedef enum {
    LAMP_PAIRING_NONE = 0,          // Normal steady operation
    LAMP_PAIRING_WAITING = 1,       // Slow pulsing (2.0s cycle) while waiting to pair
    LAMP_PAIRING_ACTIVE = 2,        // Faster pulsing (0.6s cycle) while actively pairing over BLE/PASE
    LAMP_PAIRING_FACTORY_RESET = 3, // Rapid flashing (0.25s cycle) before factory reset reboot
} lamp_pairing_state_t;

/**
 * @brief Set the visual pairing/status indicator state.
 */
void lamp_set_pairing_state(lamp_pairing_state_t state);

/**
 * @brief Query current pairing indicator state.
 */
lamp_pairing_state_t lamp_get_pairing_state(void);

/**
 * @brief Erase persisted lamp state in NVS (used during factory reset).
 */
void lamp_erase_nvs_state(void);

/**
 * @brief Query current lamp state.
 */
bool lamp_get_power(void);
uint8_t lamp_get_brightness(void);
uint16_t lamp_get_color_temp(void);

#ifdef __cplusplus
}
#endif
