#include "knob_driver.h"
#include "lamp_config.h"
#include "lamp_driver.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <algorithm>

static const char *TAG = "knob_driver";
static uint16_t s_endpoint_id = 1;

// Stock Xiaomi Lamp 1S uses 10 steps across the full range:
// Brightness table (0x3f40f5b4): {1, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100}% -> ~25 Matter levels per detent
// Color Temp table (0x3f40f5e0): {2600..5000}K (196..370 mireds) -> ~18 mireds per detent
static constexpr int16_t KNOB_BRIGHTNESS_STEP = 25;
static constexpr int32_t KNOB_COLOR_TEMP_STEP_MIREDS = 18;

// Hardware ISR state for quadrature rotary encoder (GPIO 26 & 27)
// Stock Xiaomi Lamp 1S uses a half-cycle encoder (2 transitions per physical detent, resting at 00 and 11)
static volatile uint8_t s_enc_state = 0;
static volatile int8_t s_enc_sub_accum = 0;
static volatile int32_t s_detent_delta_unpressed = 0;
static volatile int32_t s_detent_delta_pressed = 0;
static volatile bool s_btn_consumed_by_isr = false;

static void IRAM_ATTR rotary_isr_handler(void *arg)
{
    static const int8_t rot_enc_table[16] = {
         0,  1, -1,  0,
        -1,  0,  0,  1,
         1,  0,  0, -1,
         0, -1,  1,  0
    };

    uint8_t a = static_cast<uint8_t>(gpio_get_level(LAMP_ROTARY_A_PIN));
    uint8_t b = static_cast<uint8_t>(gpio_get_level(LAMP_ROTARY_B_PIN));
    uint8_t state = ((s_enc_state << 2) | (a << 1) | b) & 0x0F;
    s_enc_state = state;

    int8_t step = rot_enc_table[state];
    if (step != 0) {
        int8_t accum = s_enc_sub_accum + step;
        // Half-cycle encoder lands in a stable detent whenever A == B (00 or 11) after 2 quadrature transitions
        if (a == b && (accum >= 2 || accum <= -2)) {
            int32_t dir = (accum > 0) ? 1 : -1;
            s_enc_sub_accum = 0;

            if (gpio_get_level(LAMP_BTN_PIN) == 0) {
                s_detent_delta_pressed += dir;
                s_btn_consumed_by_isr = true;
            } else {
                s_detent_delta_unpressed += dir;
            }
        } else {
            s_enc_sub_accum = accum;
        }
    }
}

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
    esp_matter_attr_val_t val = esp_matter_nullable_uint8(new_level);
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

static void trigger_factory_reset(const char *source)
{
    ESP_LOGW(TAG, "%s detected! Flashing confirmation and initiating Matter Factory Reset...", source);
    lamp_set_pairing_state(LAMP_PAIRING_FACTORY_RESET);
    vTaskDelay(pdMS_TO_TICKS(1250)); // ~5 rapid flashes (250ms period) for visual confirmation
    lamp_erase_nvs_state();
    esp_matter::factory_reset();
}

static void knob_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Knob task started (interrupt-driven encoder, 10%% step per detent, GPIO16 reset button)");

    const uint32_t poll_interval_ms = 20;
    const TickType_t poll_delay_ticks = pdMS_TO_TICKS(poll_interval_ms) > 0 ? pdMS_TO_TICKS(poll_interval_ms) : 1;

    uint32_t button_held_ms = 0;
    uint32_t reset_btn_held_ms = 0;
    bool button_consumed_by_rotation = false;
    bool factory_reset_triggered = false;

    while (true) {
        // 0. Read Bottom Pinhole Reset Button (GPIO 16, Active LOW, stock 3s hold)
        bool reset_btn_pressed = (gpio_get_level(LAMP_RESET_BTN_PIN) == 0);
        if (reset_btn_pressed) {
            reset_btn_held_ms += poll_interval_ms;
            if (reset_btn_held_ms >= 3000 && !factory_reset_triggered) {
                factory_reset_triggered = true;
                trigger_factory_reset("Bottom Reset Button (3s hold)");
            }
        } else {
            reset_btn_held_ms = 0;
        }

        // 1. Read Knob Push Button State (GPIO 33, Active LOW)
        bool btn_pressed = (gpio_get_level(LAMP_BTN_PIN) == 0);
        if (s_btn_consumed_by_isr) {
            button_consumed_by_rotation = true;
            s_btn_consumed_by_isr = false;
        }

        if (btn_pressed) {
            button_held_ms += poll_interval_ms;

            // Long-press: hold knob for >8 seconds triggers Factory Reset
            if (button_held_ms > 8000 && !factory_reset_triggered) {
                factory_reset_triggered = true;
                trigger_factory_reset("Knob Long-Press (>8s)");
            }
        } else {
            // Button released
            if (button_held_ms >= 40 && button_held_ms <= 1500) {
                // Short click detected (between ~40ms and ~1.5s) without rotation
                if (!button_consumed_by_rotation && !factory_reset_triggered) {
                    if (lamp_get_pairing_state() == LAMP_PAIRING_WAITING) {
                        ESP_LOGI(TAG, "Knob Click during pairing wait: switching to manual control");
                        lamp_set_pairing_state(LAMP_PAIRING_NONE);
                    } else {
                        bool current_pwr = lamp_get_power();
                        bool new_pwr = !current_pwr;
                        ESP_LOGI(TAG, "Knob Click: Toggling power to %s", new_pwr ? "ON" : "OFF");
                        lamp_set_power(new_pwr);
                        update_matter_on_off(new_pwr);
                    }
                }
            }
            button_held_ms = 0;
            button_consumed_by_rotation = false;
        }

        // 2. Drain accumulated rotary encoder detents captured by hardware ISR
        int32_t delta_pressed = s_detent_delta_pressed;
        if (delta_pressed != 0) {
            s_detent_delta_pressed -= delta_pressed;
            button_consumed_by_rotation = true;

            uint16_t current_mireds = lamp_get_color_temp();
            int32_t new_mireds = static_cast<int32_t>(current_mireds) + (delta_pressed * KNOB_COLOR_TEMP_STEP_MIREDS);
            new_mireds = std::max<int32_t>(LAMP_MIN_MIREDS, std::min<int32_t>(LAMP_MAX_MIREDS, new_mireds));

            uint16_t target_mireds = static_cast<uint16_t>(new_mireds);
            if (target_mireds != current_mireds) {
                ESP_LOGI(TAG, "Knob Rotate (Held %+ld): Color Temp -> %u mireds", (long)delta_pressed, target_mireds);
                lamp_set_color_temp(target_mireds);
                update_matter_color_temp(target_mireds);
            }
        }

        int32_t delta_unpressed = s_detent_delta_unpressed;
        if (delta_unpressed != 0) {
            s_detent_delta_unpressed -= delta_unpressed;

            if (lamp_get_pairing_state() == LAMP_PAIRING_WAITING) {
                lamp_set_pairing_state(LAMP_PAIRING_NONE);
            }

            bool was_on = lamp_get_power();
            uint8_t current_lvl = lamp_get_brightness();
            int32_t new_lvl = static_cast<int32_t>(current_lvl) + (delta_unpressed * KNOB_BRIGHTNESS_STEP);
            new_lvl = std::max<int32_t>(LAMP_MIN_LEVEL, std::min<int32_t>(LAMP_MAX_LEVEL, new_lvl));

            uint8_t target_lvl = static_cast<uint8_t>(new_lvl);
            ESP_LOGI(TAG, "Knob Rotate (%+ld): Brightness -> %u", (long)delta_unpressed, target_lvl);

            lamp_set_brightness(target_lvl);
            update_matter_brightness(target_lvl);

            // If rotated while lamp was off, turn it on automatically (matching stock Xiaomi behavior)
            if (!was_on) {
                lamp_set_power(true);
                update_matter_on_off(true);
            }
        }

        vTaskDelay(poll_delay_ticks);
    }
}

esp_err_t knob_driver_init(uint16_t endpoint_id)
{
    s_endpoint_id = endpoint_id;
    ESP_LOGI(TAG, "Initializing Knob & Button GPIOs...");

    // Configure Knob Push Button (GPIO 33) and Bottom Pinhole Reset Button (GPIO 16)
    gpio_config_t btn_conf = {};
    btn_conf.pin_bit_mask = (1ULL << LAMP_BTN_PIN) | (1ULL << LAMP_RESET_BTN_PIN);
    btn_conf.mode = GPIO_MODE_INPUT;
    btn_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    btn_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    btn_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&btn_conf));

    // Configure Rotary Encoder GPIOs with hardware edge interrupts (matching stock firmware 0x40115a88)
    gpio_config_t enc_conf = {};
    enc_conf.pin_bit_mask = (1ULL << LAMP_ROTARY_A_PIN) | (1ULL << LAMP_ROTARY_B_PIN);
    enc_conf.mode = GPIO_MODE_INPUT;
    enc_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    enc_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    enc_conf.intr_type = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&enc_conf));

    // Initialize starting quadrature state from physical pins
    uint8_t init_a = static_cast<uint8_t>(gpio_get_level(LAMP_ROTARY_A_PIN));
    uint8_t init_b = static_cast<uint8_t>(gpio_get_level(LAMP_ROTARY_B_PIN));
    s_enc_state = ((init_a << 1) | init_b) & 0x03;

    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %d", isr_err);
        return isr_err;
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(LAMP_ROTARY_A_PIN, rotary_isr_handler, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(LAMP_ROTARY_B_PIN, rotary_isr_handler, NULL));

    // Spawn worker task with low background priority (priority 1) to process detents & button events
    BaseType_t ret = xTaskCreate(knob_task, "knob_task", 4096, NULL, 1, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create knob polling task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Knob driver initialized successfully");
    return ESP_OK;
}
