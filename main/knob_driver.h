#pragma once

#include <esp_err.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the rotary encoder and push button driver.
 * 
 * @param endpoint_id The Matter endpoint ID associated with the lighting device.
 */
esp_err_t knob_driver_init(uint16_t endpoint_id);

#ifdef __cplusplus
}
#endif
