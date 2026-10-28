#include "lamp_driver.h"
#include "lamp_config.h"

#include <esp_log.h>
#include <algorithm>

static const char *TAG = "lamp_driver";

static bool s_power = false;
static uint8_t s_brightness = LAMP_DEFAULT_LEVEL;
static uint16_t s_mireds = LAMP_DEFAULT_MIREDS;

static void lamp_apply_hardware(void)
{
    if (!s_power || s_brightness == 0) {
        // Turn off PWM channels first
        ledc_set_duty(LAMP_PWM_MODE, LAMP_COLD_LEDC_CH, 0);
        ledc_update_duty(LAMP_PWM_MODE, LAMP_COLD_LEDC_CH);

        ledc_set_duty(LAMP_PWM_MODE, LAMP_WARM_LEDC_CH, 0);
        ledc_update_duty(LAMP_PWM_MODE, LAMP_WARM_LEDC_CH);

        // Turn off power supply rail to eliminate quiescent power draw
        gpio_set_level(LAMP_POWER_PIN, 0);
        ESP_LOGI(TAG, "Lamp turned OFF (power rail disabled)");
        return;
    }

    // Energize power rail
    gpio_set_level(LAMP_POWER_PIN, 1);

    // Normalize brightness (0.0 to 1.0)
    float b = static_cast<float>(s_brightness) / static_cast<float>(LAMP_MAX_LEVEL);

    // Clamp color temperature between MIN and MAX mireds
    uint16_t clamped_mireds = std::max<uint16_t>(LAMP_MIN_MIREDS, std::min<uint16_t>(LAMP_MAX_MIREDS, s_mireds));

    // Calculate warm/cold mixing ratio:
    // 153 mireds (6500K) -> warm_ratio = 0.0, cold_ratio = 1.0
    // 370 mireds (2700K) -> warm_ratio = 1.0, cold_ratio = 0.0
    float warm_ratio = static_cast<float>(clamped_mireds - LAMP_MIN_MIREDS) /
                       static_cast<float>(LAMP_MAX_MIREDS - LAMP_MIN_MIREDS);
    float cold_ratio = 1.0f - warm_ratio;

    uint32_t cold_duty = static_cast<uint32_t>(LAMP_PWM_MAX_DUTY * b * cold_ratio);
    uint32_t warm_duty = static_cast<uint32_t>(LAMP_PWM_MAX_DUTY * b * warm_ratio);

    ledc_set_duty(LAMP_PWM_MODE, LAMP_COLD_LEDC_CH, cold_duty);
    ledc_update_duty(LAMP_PWM_MODE, LAMP_COLD_LEDC_CH);

    ledc_set_duty(LAMP_PWM_MODE, LAMP_WARM_LEDC_CH, warm_duty);
    ledc_update_duty(LAMP_PWM_MODE, LAMP_WARM_LEDC_CH);

    ESP_LOGI(TAG, "Lamp ON -> Brightness: %u, Mireds: %u (Cold PWM: %lu, Warm PWM: %lu)",
             s_brightness, clamped_mireds, (unsigned long)cold_duty, (unsigned long)warm_duty);
}

esp_err_t lamp_driver_init(void)
{
    ESP_LOGI(TAG, "Initializing Lamp Hardware Driver...");

    // 1. Configure Power Rail GPIO
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << LAMP_POWER_PIN);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(LAMP_POWER_PIN, 0);

    // 2. Configure LEDC Timer
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = LAMP_PWM_MODE;
    ledc_timer.timer_num = LAMP_PWM_TIMER;
    ledc_timer.duty_resolution = LAMP_PWM_RES;
    ledc_timer.freq_hz = LAMP_PWM_FREQ_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 3. Configure Cold White Channel
    ledc_channel_config_t cold_ch = {};
    cold_ch.channel = LAMP_COLD_LEDC_CH;
    cold_ch.duty = 0;
    cold_ch.gpio_num = LAMP_COLD_PIN;
    cold_ch.speed_mode = LAMP_PWM_MODE;
    cold_ch.hpoint = 0;
    cold_ch.timer_sel = LAMP_PWM_TIMER;
    ESP_ERROR_CHECK(ledc_channel_config(&cold_ch));

    // 4. Configure Warm White Channel
    ledc_channel_config_t warm_ch = {};
    warm_ch.channel = LAMP_WARM_LEDC_CH;
    warm_ch.duty = 0;
    warm_ch.gpio_num = LAMP_WARM_PIN;
    warm_ch.speed_mode = LAMP_PWM_MODE;
    warm_ch.hpoint = 0;
    warm_ch.timer_sel = LAMP_PWM_TIMER;
    ESP_ERROR_CHECK(ledc_channel_config(&warm_ch));

    ESP_LOGI(TAG, "Lamp Hardware Driver initialized successfully");
    return ESP_OK;
}

esp_err_t lamp_set_power(bool on)
{
    s_power = on;
    lamp_apply_hardware();
    return ESP_OK;
}

esp_err_t lamp_set_brightness(uint8_t level)
{
    s_brightness = std::max<uint8_t>(LAMP_MIN_LEVEL, std::min<uint8_t>(LAMP_MAX_LEVEL, level));
    if (s_power) {
        lamp_apply_hardware();
    }
    return ESP_OK;
}

esp_err_t lamp_set_color_temp(uint16_t mireds)
{
    s_mireds = std::max<uint16_t>(LAMP_MIN_MIREDS, std::min<uint16_t>(LAMP_MAX_MIREDS, mireds));
    if (s_power) {
        lamp_apply_hardware();
    }
    return ESP_OK;
}

bool lamp_get_power(void)
{
    return s_power;
}

uint8_t lamp_get_brightness(void)
{
    return s_brightness;
}

uint16_t lamp_get_color_temp(void)
{
    return s_mireds;
}
