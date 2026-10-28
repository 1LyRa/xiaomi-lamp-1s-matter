#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_ota.h>
#include <platform/CHIPDeviceLayer.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Attributes.h>

#include "lamp_config.h"
#include "lamp_driver.h"
#include "knob_driver.h"

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static const char *TAG = "app_main";

static esp_err_t app_attribute_update_cb(
    attribute::callback_type_t type,
    uint16_t endpoint_id,
    uint32_t cluster_id,
    uint32_t attribute_id,
    esp_matter_attr_val_t *val,
    void *priv_data)
{
    if (type == attribute::callback_type_t::PRE_UPDATE || type == attribute::callback_type_t::POST_UPDATE) {
        if (cluster_id == OnOff::Id) {
            if (attribute_id == OnOff::Attributes::OnOff::Id) {
                ESP_LOGI(TAG, "Matter -> OnOff: %s", val->val.b ? "TRUE" : "FALSE");
                lamp_set_power(val->val.b);
            }
        } else if (cluster_id == LevelControl::Id) {
            if (attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
                ESP_LOGI(TAG, "Matter -> Brightness: %u", val->val.u8);
                lamp_set_brightness(val->val.u8);
            }
        } else if (cluster_id == ColorControl::Id) {
            if (attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
                ESP_LOGI(TAG, "Matter -> Color Temp: %u mireds", val->val.u16);
                lamp_set_color_temp(val->val.u16);
            }
        }
    }
    return ESP_OK;
}

static esp_err_t app_identification_cb(
    identification::callback_type_t type,
    uint16_t endpoint_id,
    uint8_t effect_id,
    uint8_t effect_variant,
    void *priv_data)
{
    ESP_LOGI(TAG, "Matter identification callback: type=%d", type);
    return ESP_OK;
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Matter commissioning completed successfully!");
        break;
    default:
        break;
    }
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "Starting Xiaomi Lamp 1S Matter over Wi-Fi Firmware");
    ESP_LOGI(TAG, "Hardware: ESP32-S0WD / ESP-WROOM-32D (Unicore)");
    ESP_LOGI(TAG, "=================================================");

    // 1. Initialize NVS (required by Wi-Fi & Matter credentials)
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS truncated or updated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 2. Initialize Lamp Hardware Driver (LEDC PWM & Power Rail)
    ESP_ERROR_CHECK(lamp_driver_init());

    // 3. Create Matter Node
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    // 4. Create Color Temperature Light Endpoint
    color_temperature_light::config_t light_config;
    light_config.on_off.on_off = false;
    light_config.level_control.current_level = LAMP_DEFAULT_LEVEL;

    endpoint_t *endpoint = color_temperature_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create color temperature light endpoint");
        return;
    }

    uint16_t endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Color Temperature Light created at Endpoint ID: %u", endpoint_id);

    // Set default color temperature (250 mireds ~ 4000K)
    esp_matter_attr_val_t val = esp_matter_uint16(LAMP_DEFAULT_MIREDS);
    attribute::update(endpoint_id,
                      ColorControl::Id,
                      ColorControl::Attributes::ColorTemperatureMireds::Id,
                      &val);

    // 5. Initialize Physical Controls (Push Button + Rotary Knob)
    ESP_ERROR_CHECK(knob_driver_init(endpoint_id));

    // 6. Start Matter Stack
    ESP_ERROR_CHECK(esp_matter::start(app_event_cb));
    ESP_LOGI(TAG, "Matter stack started. Ready for commissioning!");
}
