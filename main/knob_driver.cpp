#include "knob_driver.h"
#include "lamp_config.h"
#include "lamp_driver.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <algorithm>

static const char *TAG = "knob_driver";
static uint16_t s_endpoint_id = 1;

static void update_matter_on_off(bool new_power)
{
    esp_matter_attr_val_t val = esp_matter_bool(new_power);
    esp_matter::attribute::update(s_endpoint_id,
                                  chip::app::Clusters::OnOff::Id,
                                  chip::app::Clusters::OnOff::Attributes::OnOff::Id,
                                  &val);
}

static void update_matter_brightness(uint8_t new_level)
{
    esp_matter_attr_val_t val = esp_matter_uint8(new_level);
    esp_matter::attribute::update(s_endpoint_id,
                                  chip::app::Clusters::LevelControl::Id,
                                  chip::app::Clusters::LevelControl::Attributes::CurrentLevel::Id,
                                  &val);
}

static void update_matter_color_temp(uint16_t new_mireds)
{
    esp_matter_attr_val_t val = esp_matter_uint16(new_mireds);
    esp_matter::attribute::update(s_endpoint_id,
                                  chip::app::Clusters::ColorControl::Id,
                                  chip::app::Clusters::ColorControl::Attributes::ColorTemperatureMireds::Id,
                                  &val);
}

static void knob_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Knob polling task started");

    // Rotary encoder state table
    static const int8_t rot_enc_table[] = {
        0,  1, -1,  0,
       -1,  0,  0,  1,
        1,  0,  0, -1,
        0, -1,  1,  0
    };

    uint8_t enc_val = 0;
    int8_t enc_accum = 0;

    const uint32_t poll_interval_ms = 10;
    const TickType_t poll_delay_ticks = pdMS_TO_TICKS(poll_interval_ms) > 0 ? pdMS_TO_TICKS(poll_interval_ms) : 1;

    uint32_t button_held_ms = 0;
    bool button_consumed_by_rotation = false;
    bool factory_reset_triggered = false;

    while (true) {
        // 1. Read Button State (Active LOW)
        bool btn_pressed = (gpio_get_level(LAMP_BTN_PIN) == 0);

        if (btn_pressed) {
            button_held_ms += poll_interval_ms;

            // Long-press: hold for >8 seconds triggers Factory Reset
            if (button_held_ms > 8000 && !factory_reset_triggered) {
                factory_reset_triggered = true;
                ESP_LOGW(TAG, "Long-press detected! Initiating Matter Factory Reset...");
                esp_matter::factory_reset();
            }
        } else {
            // Button released
            if (button_held_ms >= 40 && button_held_ms <= 1500) {
                // Short click detected (between ~40ms and ~1.5s) without rotation
                if (!button_consumed_by_rotation && !factory_reset_triggered) {
                    bool current_pwr = lamp_get_power();
                    bool new_pwr = !current_pwr;
                    ESP_LOGI(TAG, "Knob Click: Toggling power to %s", new_pwr ? "ON" : "OFF");
                    update_matter_on_off(new_pwr);
                }
            }
            button_held_ms = 0;
            button_consumed_by_rotation = false;
            factory_reset_triggered = false;
        }

        // 2. Read Rotary Encoder (Channels A and B)
        uint8_t a = gpio_get_level(LAMP_ROTARY_A_PIN);
        uint8_t b = gpio_get_level(LAMP_ROTARY_B_PIN);
        enc_val = ((enc_val << 2) | (a << 1) | b) & 0x0F;
        int8_t step = rot_enc_table[enc_val];

        if (step != 0) {
            enc_accum += step;

            // Most encoders complete one detent / click every 4 sub-steps
            if (enc_accum >= 4 || enc_accum <= -4) {
                int8_t direction = (enc_accum > 0) ? 1 : -1;
                enc_accum = 0;

                if (btn_pressed) {
                    // Holding button while turning -> Adjust Color Temperature
                    button_consumed_by_rotation = true;
                    uint16_t current_mireds = lamp_get_color_temp();
                    int32_t new_mireds = current_mireds + (direction * 15);
                    new_mireds = std::max<int32_t>(LAMP_MIN_MIREDS, std::min<int32_t>(LAMP_MAX_MIREDS, new_mireds));

                    ESP_LOGI(TAG, "Knob Rotate (Held): Color Temp -> %ld mireds", (long)new_mireds);
                    update_matter_color_temp(static_cast<uint16_t>(new_mireds));
                } else {
                    // Turning without holding button -> Adjust Brightness
                    uint8_t current_lvl = lamp_get_brightness();
                    int16_t new_lvl = current_lvl + (direction * 15);
                    new_lvl = std::max<int16_t>(LAMP_MIN_LEVEL, std::min<int16_t>(LAMP_MAX_LEVEL, new_lvl));

                    ESP_LOGI(TAG, "Knob Rotate: Brightness -> %d", new_lvl);
                    update_matter_brightness(static_cast<uint8_t>(new_lvl));

                    // If turned while lamp was off, turn it on automatically
                    if (!lamp_get_power()) {
                        update_matter_on_off(true);
                    }
                }
            }
        }

        vTaskDelay(poll_delay_ticks);
    }
}

esp_err_t knob_driver_init(uint16_t endpoint_id)
{
    s_endpoint_id = endpoint_id;
    ESP_LOGI(TAG, "Initializing Knob & Button GPIOs...");

    // Configure Button GPIO
    gpio_config_t btn_conf = {};
    btn_conf.pin_bit_mask = (1ULL << LAMP_BTN_PIN);
    btn_conf.mode = GPIO_MODE_INPUT;
    btn_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    btn_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    btn_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&btn_conf));

    // Configure Rotary Encoder GPIOs
    gpio_config_t enc_conf = {};
    enc_conf.pin_bit_mask = (1ULL << LAMP_ROTARY_A_PIN) | (1ULL << LAMP_ROTARY_B_PIN);
    enc_conf.mode = GPIO_MODE_INPUT;
    enc_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    enc_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    enc_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&enc_conf));

    // Spawn polling task with low background priority (priority 1) to never starve FreeRTOS IDLE or Matter
    BaseType_t ret = xTaskCreate(knob_task, "knob_task", 4096, NULL, 1, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create knob polling task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Knob driver initialized successfully");
    return ESP_OK;
}
