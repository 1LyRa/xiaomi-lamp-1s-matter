#include "lamp_driver.h"
#include "lamp_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <algorithm>
#include <cmath>

static const char *TAG = "lamp_driver";

// Yeelight / Xiaomi Mi Desk Lamp 1S default smooth transition duration (500ms)
static constexpr int64_t LAMP_POWER_FADE_DURATION_US = 500 * 1000;
// Responsive transition duration for rotary knob / slider adjustments while on (180ms)
static constexpr int64_t LAMP_ADJUST_FADE_DURATION_US = 180 * 1000;

static bool s_power = false;
static uint8_t s_brightness = LAMP_DEFAULT_LEVEL;
static uint16_t s_mireds = LAMP_DEFAULT_MIREDS;

static TaskHandle_t s_fade_task_handle = NULL;

// Perceptual brightness [0.0 .. 1.0] and warm ratio [0.0 .. 1.0] interpolation state
static volatile float s_start_b = 0.0f;
static volatile float s_start_warm_ratio = 0.5f;
static volatile float s_target_b = 0.0f;
static volatile float s_target_warm_ratio = 0.5f;
static volatile bool s_target_power = false;
static volatile int64_t s_fade_elapsed_us = 0;
static volatile int64_t s_fade_last_frame_us = 0;
static volatile int64_t s_fade_duration_us = LAMP_POWER_FADE_DURATION_US;
static volatile bool s_in_power_fade = false;

static float s_current_b = 0.0f;
static float s_current_warm_ratio = 0.5f;
static bool s_rail_powered = false;

static volatile lamp_pairing_state_t s_pairing_state = LAMP_PAIRING_NONE;
static int64_t s_pulse_elapsed_us = 0;

// Last saved state to avoid duplicate NVS flash writes
static bool s_nvs_last_power = false;
static uint8_t s_nvs_last_brightness = 0;
static uint16_t s_nvs_last_mireds = 0;

void lamp_erase_nvs_state(void)
{
    nvs_handle_t handle;
    if (nvs_open("lamp_state", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGI(TAG, "Erased lamp_state NVS namespace for factory reset");
    }
}

static void lamp_save_state_to_nvs(void)
{
    if (s_power == s_nvs_last_power &&
        s_brightness == s_nvs_last_brightness &&
        s_mireds == s_nvs_last_mireds) {
        return; // Nothing changed, skip writing
    }

    nvs_handle_t handle;
    if (nvs_open("lamp_state", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "power", s_power ? 1 : 0);
        nvs_set_u8(handle, "level", s_brightness);
        nvs_set_u16(handle, "mireds", s_mireds);
        nvs_commit(handle);
        nvs_close(handle);

        s_nvs_last_power = s_power;
        s_nvs_last_brightness = s_brightness;
        s_nvs_last_mireds = s_mireds;

        ESP_LOGI(TAG, "Persisted state to NVS: Power=%d, Brightness=%u, Mireds=%u",
                 s_power, s_brightness, s_mireds);
    }
}

static void lamp_load_state_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open("lamp_state", NVS_READONLY, &handle) == ESP_OK) {
        uint8_t pwr = 0;
        uint8_t lvl = 0;
        uint16_t mrd = 0;

        if (nvs_get_u8(handle, "power", &pwr) == ESP_OK) {
            s_power = (pwr != 0);
        }
        if (nvs_get_u8(handle, "level", &lvl) == ESP_OK && lvl >= LAMP_MIN_LEVEL) {
            s_brightness = lvl;
        }
        if (nvs_get_u16(handle, "mireds", &mrd) == ESP_OK && mrd >= LAMP_MIN_MIREDS && mrd <= LAMP_MAX_MIREDS) {
            s_mireds = mrd;
        }
        nvs_close(handle);

        s_nvs_last_power = s_power;
        s_nvs_last_brightness = s_brightness;
        s_nvs_last_mireds = s_mireds;

        ESP_LOGI(TAG, "Loaded state from NVS: Power=%d, Brightness=%u, Mireds=%u",
                 s_power, s_brightness, s_mireds);
    }
}

static void lamp_apply_pwm(float perceptual_b, float warm_ratio)
{
    float total_duty = 0.0f;
    if (perceptual_b > 0.001f) {
        // Quadratic gamma curve maps perceptual brightness [0..1] to hardware PWM duty [0..1023]
        total_duty = static_cast<float>(LAMP_PWM_MAX_DUTY) * (perceptual_b * perceptual_b);
    }

    uint32_t warm_duty = static_cast<uint32_t>(std::round(std::max(0.0f, total_duty * warm_ratio)));
    uint32_t cold_duty = static_cast<uint32_t>(std::round(std::max(0.0f, total_duty * (1.0f - warm_ratio))));

    ledc_set_duty(LAMP_PWM_MODE, LAMP_COLD_LEDC_CH, cold_duty);
    ledc_update_duty(LAMP_PWM_MODE, LAMP_COLD_LEDC_CH);

    ledc_set_duty(LAMP_PWM_MODE, LAMP_WARM_LEDC_CH, warm_duty);
    ledc_update_duty(LAMP_PWM_MODE, LAMP_WARM_LEDC_CH);
}

static void lamp_recompute_targets(bool is_power_change)
{
    uint16_t clamped_mireds = std::max<uint16_t>(LAMP_MIN_MIREDS, std::min<uint16_t>(LAMP_MAX_MIREDS, s_mireds));
    float warm_ratio = static_cast<float>(clamped_mireds - LAMP_MIN_MIREDS) /
                       static_cast<float>(LAMP_MAX_MIREDS - LAMP_MIN_MIREDS);

    if (!s_power || s_brightness == 0) {
        s_target_power = false;
        s_target_b = 0.0f;
        s_target_warm_ratio = warm_ratio;
    } else {
        s_target_power = true;
        // Map Matter level [1..254] to perceptual brightness [0.15 .. 1.00]
        // 0.15^2 * 1023 = ~23 duty (~2.2%), ensuring level 1 is visibly lit above LED forward voltage
        float norm_level = static_cast<float>(s_brightness - LAMP_MIN_LEVEL) /
                           static_cast<float>(LAMP_MAX_LEVEL - LAMP_MIN_LEVEL);
        constexpr float min_perceptual_b = 0.15f;
        s_target_b = min_perceptual_b + (1.0f - min_perceptual_b) * norm_level;
        s_target_warm_ratio = warm_ratio;

        // When turning ON from completely off, lock the starting color ratio to target so it fades on cleanly
        if (s_current_b <= 0.001f) {
            s_current_warm_ratio = warm_ratio;
        }
    }

    s_start_b = s_current_b;
    s_start_warm_ratio = s_current_warm_ratio;
    s_fade_elapsed_us = 0;
    s_fade_last_frame_us = esp_timer_get_time();

    if (is_power_change || s_in_power_fade) {
        s_in_power_fade = true;
        s_fade_duration_us = LAMP_POWER_FADE_DURATION_US; // 500ms Yeelight smooth power fade
    } else {
        s_fade_duration_us = LAMP_ADJUST_FADE_DURATION_US; // 180ms responsive knob/app adjustment
    }

    if (s_fade_task_handle) {
        xTaskNotifyGive(s_fade_task_handle);
    }
}

static void lamp_fade_task(void *pvParameters)
{
    const TickType_t frame_delay = 1; // 1 tick (10ms at 100Hz) = 100 FPS smooth interpolation
    const int64_t max_frame_dt_us = 20000; // Cap single-frame delta at 20ms so RF calibration pauses never cause jumps

    while (true) {
        lamp_pairing_state_t pairing_state = s_pairing_state;

        if (pairing_state != LAMP_PAIRING_NONE) {
            if (!s_rail_powered) {
                gpio_set_level(LAMP_POWER_PIN, 1);
                s_rail_powered = true;
                vTaskDelay(pdMS_TO_TICKS(25) > 0 ? pdMS_TO_TICKS(25) : 2);
                s_fade_last_frame_us = esp_timer_get_time();
            }

            int64_t now_us = esp_timer_get_time();
            int64_t dt_us = now_us - s_fade_last_frame_us;
            s_fade_last_frame_us = now_us;
            if (dt_us < 0) dt_us = 0;
            if (dt_us > max_frame_dt_us) dt_us = max_frame_dt_us;

            s_pulse_elapsed_us += dt_us;

            int64_t period_us = 2000 * 1000; // Default: 2.0s slow breathing while waiting to pair
            float min_b = 0.18f;
            float max_b = 0.75f;

            if (pairing_state == LAMP_PAIRING_ACTIVE) {
                period_us = 550 * 1000;      // 0.55s faster pulsing while actively pairing
                min_b = 0.15f;
                max_b = 0.88f;
            } else if (pairing_state == LAMP_PAIRING_FACTORY_RESET) {
                period_us = 250 * 1000;      // 0.25s rapid flashing for factory reset confirmation
                min_b = 0.05f;
                max_b = 0.95f;
            }

            float phase = static_cast<float>(s_pulse_elapsed_us % period_us) / static_cast<float>(period_us);
            // Smooth cosine wave: 0 -> 1 -> 0 over each period
            float wave = 0.5f * (1.0f - std::cos(phase * 2.0f * 3.14159265f));
            s_current_b = min_b + (max_b - min_b) * wave;
            s_current_warm_ratio = 0.45f; // Neutral ~4000K white during status indication

            lamp_apply_pwm(s_current_b, s_current_warm_ratio);
            vTaskDelay(frame_delay);
            continue;
        }

        // When turning ON from cold, energize the buck power rail first and let voltage stabilize at 0 duty
        if (s_target_power && !s_rail_powered) {
            gpio_set_level(LAMP_POWER_PIN, 1);
            s_rail_powered = true;
            vTaskDelay(pdMS_TO_TICKS(25) > 0 ? pdMS_TO_TICKS(25) : 2);
            // Reset fade timer after rail stabilization so the full 500ms fade is visible
            s_fade_elapsed_us = 0;
            s_fade_last_frame_us = esp_timer_get_time();
        }

        int64_t now_us = esp_timer_get_time();
        int64_t dt_us = now_us - s_fade_last_frame_us;
        s_fade_last_frame_us = now_us;
        if (dt_us < 0) dt_us = 0;
        if (dt_us > max_frame_dt_us) dt_us = max_frame_dt_us;

        s_fade_elapsed_us += dt_us;
        int64_t elapsed_us = s_fade_elapsed_us;
        int64_t duration_us = s_fade_duration_us;

        if (elapsed_us >= duration_us) {
            s_current_b = s_target_b;
            s_current_warm_ratio = s_target_warm_ratio;
            s_in_power_fade = false;

            lamp_apply_pwm(s_current_b, s_current_warm_ratio);

            if (!s_target_power && s_rail_powered) {
                // Wait briefly for PWM signals to settle at zero before disabling buck power rail
                vTaskDelay(pdMS_TO_TICKS(20) > 0 ? pdMS_TO_TICKS(20) : 2);
                gpio_set_level(LAMP_POWER_PIN, 0);
                s_rail_powered = false;
            }

            // Persist steady state to NVS (avoids flash wear during transitions)
            lamp_save_state_to_nvs();

            // Sleep until the next state change
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            s_fade_last_frame_us = esp_timer_get_time();
            continue;
        }

        // Power ON/OFF uses a cosine S-curve (Ease-In-Out); knob/app adjustments use a fast Ease-Out curve
        // so continuous knob rotation responds immediately without S-curve start lag.
        float progress = std::max(0.0f, std::min(1.0f, static_cast<float>(elapsed_us) / static_cast<float>(duration_us)));
        float ease = s_in_power_fade
            ? (0.5f * (1.0f - std::cos(progress * 3.14159265f)))
            : (1.0f - (1.0f - progress) * (1.0f - progress));

        s_current_b = s_start_b + (s_target_b - s_start_b) * ease;
        s_current_warm_ratio = s_start_warm_ratio + (s_target_warm_ratio - s_start_warm_ratio) * ease;

        lamp_apply_pwm(s_current_b, s_current_warm_ratio);

        vTaskDelay(frame_delay);
    }
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
    s_rail_powered = false;

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

    // 5. Restore previous state from NVS
    lamp_load_state_from_nvs();

    // 6. Create smoothing fade task at Priority 18 so Matter/Wi-Fi init (Priority 5) never starves PWM fading
    BaseType_t ret = xTaskCreate(lamp_fade_task, "lamp_fade_task", 3072, NULL, 18, &s_fade_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create lamp fade task");
        return ESP_FAIL;
    }

    // 7. If lamp was ON before reboot, immediately resume lighting with smooth 500ms fade-in.
    // Let the 500ms fade finish uninterrupted before Wi-Fi/BLE PHY calibration locks the CPU/flash.
    if (s_power) {
        lamp_recompute_targets(true);
        while (s_in_power_fade) {
            vTaskDelay(pdMS_TO_TICKS(20) > 0 ? pdMS_TO_TICKS(20) : 2);
        }
    }

    ESP_LOGI(TAG, "Lamp Hardware Driver initialized (state restoration & 500ms S-curve fade enabled)");
    return ESP_OK;
}

void lamp_set_pairing_state(lamp_pairing_state_t state)
{
    if (s_pairing_state == state) {
        return;
    }
    ESP_LOGI(TAG, "Pairing indicator state: %d -> %d", (int)s_pairing_state, (int)state);
    s_pairing_state = state;
    s_pulse_elapsed_us = 0;
    s_fade_last_frame_us = esp_timer_get_time();

    if (state == LAMP_PAIRING_WAITING || state == LAMP_PAIRING_ACTIVE) {
        // Ensure lamp stays ON once pairing finishes
        s_power = true;
    } else if (state == LAMP_PAIRING_NONE) {
        // Transition smoothly from current pulse brightness to steady target state
        lamp_recompute_targets(true);
        return;
    }

    if (s_fade_task_handle) {
        xTaskNotifyGive(s_fade_task_handle);
    }
}

lamp_pairing_state_t lamp_get_pairing_state(void)
{
    return s_pairing_state;
}

esp_err_t lamp_set_power(bool on)
{
    if (s_pairing_state == LAMP_PAIRING_WAITING) {
        s_pairing_state = LAMP_PAIRING_NONE;
    }
    if (s_power == on && s_pairing_state == LAMP_PAIRING_NONE && !s_in_power_fade) {
        return ESP_OK;
    }
    s_power = on;
    lamp_recompute_targets(true);
    return ESP_OK;
}

esp_err_t lamp_set_brightness(uint8_t level)
{
    uint8_t clamped = std::max<uint8_t>(LAMP_MIN_LEVEL, std::min<uint8_t>(LAMP_MAX_LEVEL, level));
    if (s_brightness == clamped && !s_in_power_fade) {
        return ESP_OK;
    }
    s_brightness = clamped;
    if (s_power) {
        lamp_recompute_targets(false);
    }
    return ESP_OK;
}

esp_err_t lamp_set_color_temp(uint16_t mireds)
{
    uint16_t clamped = std::max<uint16_t>(LAMP_MIN_MIREDS, std::min<uint16_t>(LAMP_MAX_MIREDS, mireds));
    if (s_mireds == clamped && !s_in_power_fade) {
        return ESP_OK;
    }
    s_mireds = clamped;
    if (s_power) {
        lamp_recompute_targets(false);
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
