#include <esp_attr.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_providers.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi_types.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/CommissionableDataProvider.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/support/logging/CHIPLogging.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Attributes.h>

#include "lamp_config.h"
#include "lamp_driver.h"
#include "knob_driver.h"

extern "C" {
int ble_gap_adv_active(void);
int ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason);
}

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

namespace esp_matter::attribute {
esp_err_t erase_val_in_nvs(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id);
}

static const char *TAG = "app_main";
static volatile bool s_ble_subscribed = false;
static volatile int32_t s_ble_adv_cooldown_ms = 0;
static char s_serial_str[16] = "5448E66AE90F";
static httpd_handle_t s_ota_server = nullptr;

// Circular buffer for runtime web logging (/logs) and persistent RTC errors/warnings (/errors)
#define WEB_LOG_BUFFER_SIZE 32768
#define WEB_ERR_BUFFER_SIZE 7680
#define WEB_ERR_RTC_MAGIC   0x4C414D505F455252ULL

struct RtcErrorLogStore {
    uint64_t magic;
    uint64_t total;
    size_t len;
    char buf[WEB_ERR_BUFFER_SIZE];
};

static char *s_web_log_buf = nullptr;
static size_t s_web_log_len = 0;
static uint64_t s_web_log_total = 0;
static RTC_NOINIT_ATTR RtcErrorLogStore s_rtc_err_store;
static bool s_web_err_at_line_start = true;
static bool s_web_err_capturing_line = false;
static portMUX_TYPE s_web_log_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void ring_buf_append_byte(char *buf, size_t buf_size, uint64_t *total, size_t *len, char c)
{
    if (!buf) {
        return;
    }
    buf[(*total) % buf_size] = c;
    (*total)++;
    if (*len < buf_size) {
        (*len)++;
    }
}

static bool is_error_or_warn_line(const char *str, int len)
{
    int idx = 0;
    if (len >= 4 && str[0] == '\033' && str[1] == '[') {
        idx = 2;
        while (idx < len && str[idx] != 'm') {
            idx++;
        }
        if (idx < len && str[idx] == 'm') {
            idx++;
        }
    }
    return (idx + 3 <= len) &&
           (str[idx] == 'E' || str[idx] == 'W') &&
           str[idx + 1] == ' ' &&
           str[idx + 2] == '(';
}

static int web_log_vprintf(const char *format, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);
    int ret = vprintf(format, args_copy);
    va_end(args_copy);

    char temp[256];
    int len = vsnprintf(temp, sizeof(temp), format, args);
    if (len > 0) {
        if (len >= (int)sizeof(temp)) {
            len = sizeof(temp) - 1;
            temp[len - 1] = '\n';
        }
        portENTER_CRITICAL(&s_web_log_mux);
        for (int i = 0; i < len; ++i) {
            char c = temp[i];
            ring_buf_append_byte(s_web_log_buf, WEB_LOG_BUFFER_SIZE, &s_web_log_total, &s_web_log_len, c);

            if (s_web_err_at_line_start) {
                if (c == '\r' || c == '\n') {
                    continue;
                }
                s_web_err_capturing_line = is_error_or_warn_line(temp + i, len - i);
                s_web_err_at_line_start = false;
            }
            if (s_web_err_capturing_line) {
                ring_buf_append_byte(s_rtc_err_store.buf, WEB_ERR_BUFFER_SIZE,
                                     &s_rtc_err_store.total, &s_rtc_err_store.len, c);
            }
            if (c == '\n') {
                s_web_err_at_line_start = true;
                s_web_err_capturing_line = false;
            }
        }
        portEXIT_CRITICAL(&s_web_log_mux);
    }
    return ret;
}

static bool is_noisy_chip_progress(const char *module, const char *formatted)
{
    if (!module || !formatted) {
        return false;
    }
    if (strcmp(module, "EM") == 0) {
        return (strstr(formatted, "Msg Retransmission to ") != nullptr &&
                strstr(formatted, "scheduled for ") != nullptr);
    }
    if (strcmp(module, "DIS") == 0) {
        return (strstr(formatted, "score has not improved:") != nullptr ||
                strstr(formatted, "Checking node lookup status") != nullptr ||
                strstr(formatted, "Keeping DNSSD lookup active") != nullptr);
    }
    return false;
}

static void chip_log_redirect_cb(const char *module, uint8_t category, const char *msg, va_list args)
{
    char tag[16];
    snprintf(tag, sizeof(tag), "chip[%s]", module ? module : "");

    char formatted[224];
    vsnprintf(formatted, sizeof(formatted), msg, args);

    if (category != chip::Logging::kLogCategory_Error && is_noisy_chip_progress(module, formatted)) {
        return;
    }

    switch (category) {
    case chip::Logging::kLogCategory_Error:
        ESP_LOGE(tag, "%s", formatted);
        break;
    case chip::Logging::kLogCategory_Progress:
        ESP_LOGI(tag, "%s", formatted);
        break;
    case chip::Logging::kLogCategory_Detail:
    default:
        ESP_LOGD(tag, "%s", formatted);
        break;
    }
}

// Custom CommissionableDataProvider matching the physical Matter QR sticker:
//   QR Payload:  MT:QDOA2KGB00SUJN33U00
//   Manual Code: 1475-062-2865
//   Discriminator: 1696 (0x6A0)
//   Passcode:      37468562
class LampCommissionableDataProvider : public chip::DeviceLayer::CommissionableDataProvider
{
public:
    static constexpr uint16_t kDiscriminator  = 1696;
    static constexpr uint32_t kPasscode       = 37468562;
    static constexpr uint32_t kIterationCount = 1000;

    CHIP_ERROR GetSetupDiscriminator(uint16_t &setupDiscriminator) override
    {
        setupDiscriminator = kDiscriminator;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR SetSetupDiscriminator(uint16_t setupDiscriminator) override
    {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    CHIP_ERROR GetSpake2pIterationCount(uint32_t &iterationCount) override
    {
        iterationCount = kIterationCount;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &saltBuf) override
    {
        static const uint8_t kSalt[] = "SPAKE2P Key Salt";
        constexpr size_t kSaltLen    = sizeof(kSalt) - 1;
        if (saltBuf.size() < kSaltLen) {
            return CHIP_ERROR_BUFFER_TOO_SMALL;
        }
        memcpy(saltBuf.data(), kSalt, kSaltLen);
        saltBuf.reduce_size(kSaltLen);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR InitVerifier()
    {
        if (!mVerifierInitialized) {
            static const uint8_t kSalt[] = "SPAKE2P Key Salt";
            chip::Crypto::Spake2pVerifier verifier;
            CHIP_ERROR err = verifier.Generate(kIterationCount, chip::ByteSpan(kSalt, sizeof(kSalt) - 1), kPasscode);
            if (err != CHIP_NO_ERROR) {
                return err;
            }
            chip::MutableByteSpan serializedSpan(mSerializedVerifier, sizeof(mSerializedVerifier));
            err = verifier.Serialize(serializedSpan);
            if (err != CHIP_NO_ERROR) {
                return err;
            }
            mVerifierInitialized = true;
        }
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan &verifierBuf, size_t &outVerifierLen) override
    {
        CHIP_ERROR err = InitVerifier();
        if (err != CHIP_NO_ERROR) {
            return err;
        }

        outVerifierLen = chip::Crypto::kSpake2p_VerifierSerialized_Length;
        if (verifierBuf.size() < outVerifierLen) {
            return CHIP_ERROR_BUFFER_TOO_SMALL;
        }
        memcpy(verifierBuf.data(), mSerializedVerifier, outVerifierLen);
        verifierBuf.reduce_size(outVerifierLen);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSetupPasscode(uint32_t &setupPasscode) override
    {
        setupPasscode = kPasscode;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR SetSetupPasscode(uint32_t setupPasscode) override
    {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

private:
    bool mVerifierInitialized = false;
    uint8_t mSerializedVerifier[chip::Crypto::kSpake2p_VerifierSerialized_Length] = { 0 };
};

static LampCommissionableDataProvider s_lamp_commissionable_data;

static esp_err_t ota_status_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    uint64_t uptime_s = static_cast<uint64_t>(esp_timer_get_time() / 1000000ULL);
    unsigned long days = static_cast<unsigned long>(uptime_s / 86400ULL);
    unsigned long hours = static_cast<unsigned long>((uptime_s % 86400ULL) / 3600ULL);
    unsigned long mins = static_cast<unsigned long>((uptime_s % 3600ULL) / 60ULL);
    unsigned long secs = static_cast<unsigned long>(uptime_s % 60ULL);
    unsigned long free_kb = static_cast<unsigned long>(esp_get_free_heap_size() / 1024U);
    unsigned long min_free_kb = static_cast<unsigned long>(esp_get_minimum_free_heap_size() / 1024U);

    char html[1536];
    int len = snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Xiaomi Lamp 1S OTA</title>"
        "<style>body{font-family:system-ui,sans-serif;background:#121212;color:#eee;max-width:460px;margin:40px auto;padding:20px;border-radius:12px;background:#1e1e1e}"
        "button,input{margin-top:12px;padding:10px;width:100%%;box-sizing:border-box;border-radius:6px;border:1px solid #444;background:#2c2c2c;color:#fff}"
        "button{background:#ff6700;border:none;font-weight:bold;cursor:pointer}a{color:#ff6700;text-decoration:none}</style></head><body>"
        "<h2>Xiaomi Mi LED Desk Lamp 1S</h2>"
        "<p><b>Model:</b> MJTD01SYL<br><b>Firmware Version:</b> %s<br><b>Active Partition:</b> %s<br><b>Serial:</b> %s<br>"
        "<b>Uptime:</b> %lud %02lu:%02lu:%02lu<br><b>Free Heap:</b> %lu KB (min: %lu KB)</p>"
        "<p><a href='/logs' target='_blank'><b>View Live Device Logs (/logs)</b></a><br>"
        "<a href='/errors' target='_blank'><b>View Error &amp; Warning Logs (/errors)</b></a></p>"
        "<form method='POST' action='/ota' enctype='application/octet-stream' onsubmit=\""
        "event.preventDefault();var f=document.getElementById('f').files[0];if(!f)return;"
        "var s=document.getElementById('s');s.innerText='Uploading & flashing ('+f.size+' bytes)...';"
        "fetch('/ota',{method:'POST',body:f}).then(r=>r.text()).then(t=>{s.innerText=t;}).catch(e=>{s.innerText='Error: '+e;});\">"
        "<input type='file' id='f' accept='.bin' required>"
        "<button type='submit'>Flash OTA Firmware (.bin)</button></form>"
        "<p id='s'></p></body></html>",
        app_desc ? app_desc->version : "unknown",
        running ? running->label : "unknown",
        s_serial_str,
        days, hours, mins, secs,
        free_kb, min_free_kb);
    if (len < 0) {
        len = 0;
    } else if (len >= (int)sizeof(html)) {
        len = sizeof(html) - 1;
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, len);
}

static esp_err_t stream_ring_buffer_response(
    httpd_req_t *req,
    const char *buf,
    size_t buf_size,
    const uint64_t *total_ptr,
    const size_t *len_ptr,
    const char *empty_msg)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");

    uint64_t read_pos = 0;
    portENTER_CRITICAL(&s_web_log_mux);
    uint64_t total = *total_ptr;
    size_t cur_len = *len_ptr;
    if (!buf || cur_len == 0) {
        portEXIT_CRITICAL(&s_web_log_mux);
        if (empty_msg != nullptr) {
            return httpd_resp_sendstr(req, empty_msg);
        }
        return httpd_resp_send_chunk(req, nullptr, 0);
    }
    read_pos = total - cur_len;
    if (read_pos > 0) {
        uint64_t scan = read_pos;
        while (scan < total) {
            char ch = buf[scan % buf_size];
            scan++;
            if (ch == '\n') {
                if (scan < total) {
                    read_pos = scan;
                }
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_web_log_mux);

    char chunk[1024];
    while (true) {
        size_t to_copy = 0;
        portENTER_CRITICAL(&s_web_log_mux);
        total = *total_ptr;
        cur_len = *len_ptr;
        uint64_t earliest = total - cur_len;
        if (read_pos < earliest) {
            read_pos = earliest;
        }
        if (read_pos < total) {
            uint64_t avail = total - read_pos;
            to_copy = (avail < sizeof(chunk)) ? static_cast<size_t>(avail) : sizeof(chunk);
            for (size_t i = 0; i < to_copy; ++i) {
                chunk[i] = buf[(read_pos + i) % buf_size];
            }
            read_pos += to_copy;
        }
        portEXIT_CRITICAL(&s_web_log_mux);

        if (to_copy == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, chunk, to_copy) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t ota_logs_get_handler(httpd_req_t *req)
{
    return stream_ring_buffer_response(req, s_web_log_buf, WEB_LOG_BUFFER_SIZE,
                                       &s_web_log_total, &s_web_log_len, nullptr);
}

static esp_err_t ota_errors_get_handler(httpd_req_t *req)
{
    return stream_ring_buffer_response(req, s_rtc_err_store.buf, WEB_ERR_BUFFER_SIZE,
                                       &s_rtc_err_store.total, &s_rtc_err_store.len,
                                       "No errors or warnings recorded since boot.\n");
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition found");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Starting Wi-Fi OTA update to partition '%s' (offset 0x%lx, content_len=%d)...",
             update_partition->label, (unsigned long)update_partition->address, req->content_len);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA receive interrupted");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, recv_len);
        if (err != ESP_OK) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write failed");
            return ESP_FAIL;
        }
        remaining -= recv_len;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA image validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Wi-Fi OTA update succeeded! Rebooting into '%s'...", update_partition->label);
    httpd_resp_sendstr(req, "OTA Update Successful! Lamp is rebooting into new firmware...\n");

    lamp_set_pairing_state(LAMP_PAIRING_FACTORY_RESET);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static void start_local_ota_server()
{
    if (s_ota_server) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 6144;
    if (httpd_start(&s_ota_server, &config) == ESP_OK) {
        static const httpd_uri_t get_root = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = ota_status_get_handler,
            .user_ctx = nullptr
        };
        static const httpd_uri_t post_ota = {
            .uri      = "/ota",
            .method   = HTTP_POST,
            .handler  = ota_post_handler,
            .user_ctx = nullptr
        };
        static const httpd_uri_t get_logs = {
            .uri      = "/logs",
            .method   = HTTP_GET,
            .handler  = ota_logs_get_handler,
            .user_ctx = nullptr
        };
        static const httpd_uri_t get_errors = {
            .uri      = "/errors",
            .method   = HTTP_GET,
            .handler  = ota_errors_get_handler,
            .user_ctx = nullptr
        };
        httpd_register_uri_handler(s_ota_server, &get_root);
        httpd_register_uri_handler(s_ota_server, &get_logs);
        httpd_register_uri_handler(s_ota_server, &get_errors);
        httpd_register_uri_handler(s_ota_server, &post_ota);
        ESP_LOGI(TAG, "Local Wi-Fi OTA & Log Server started on port 80 (GET /, GET /logs, GET /errors, POST /ota)");
    }
}

static esp_err_t app_attribute_update_cb(
    attribute::callback_type_t type,
    uint16_t endpoint_id,
    uint32_t cluster_id,
    uint32_t attribute_id,
    esp_matter_attr_val_t *val,
    void *priv_data)
{
    if (type == attribute::callback_type_t::POST_UPDATE) {
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

static bool is_device_commissioned()
{
    return chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
}

static void remote_factory_reset_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    lamp_erase_nvs_state();
    esp_matter::factory_reset();
    vTaskDelete(nullptr);
}

static esp_err_t app_identification_cb(
    identification::callback_type_t type,
    uint16_t endpoint_id,
    uint8_t effect_id,
    uint8_t effect_variant,
    void *priv_data)
{
    ESP_LOGI(TAG, "Matter identification callback: type=%d, effect=%u", type, effect_id);
    if (type == identification::callback_type_t::START) {
        lamp_set_pairing_state(LAMP_PAIRING_ACTIVE);
    } else if (type == identification::callback_type_t::STOP) {
        lamp_set_pairing_state(is_device_commissioned() ? LAMP_PAIRING_NONE : LAMP_PAIRING_WAITING);
    }
    return ESP_OK;
}

static void reopen_commissioning_window_work(intptr_t arg)
{
    if (!is_device_commissioned()) {
        auto &cwm = chip::Server::GetInstance().GetCommissioningWindowManager();
        if (!cwm.IsCommissioningWindowOpen()) {
            ESP_LOGW(TAG, "Reopening commissioning window after un-armed BLE disconnect...");
            cwm.CloseCommissioningWindow();
            (void)cwm.OpenBasicCommissioningWindow();
        }
        (void)chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(true);
    }
}

static void on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
    start_local_ota_server();
    ESP_LOGI(TAG, "Wi-Fi IPv4 Acquired: " IPSTR " -> Local OTA Web UI: http://" IPSTR "/",
             IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.ip));
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    using namespace chip::DeviceLayer;
    SetCommissionableDataProvider(&s_lamp_commissionable_data);

    switch (event->Type) {
    case DeviceEventType::kCommissioningWindowOpened:
        if (!is_device_commissioned()) {
            if (s_ble_adv_cooldown_ms > 0) {
                (void)chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false);
            }
            if (lamp_get_pairing_state() != LAMP_PAIRING_ACTIVE) {
                ESP_LOGI(TAG, "Uncommissioned & Commissioning Window Open -> Slow breathing indicator");
                lamp_set_pairing_state(LAMP_PAIRING_WAITING);
            }
        }
        break;

    case DeviceEventType::kCHIPoBLEConnectionEstablished:
    case DeviceEventType::kCHIPoBLESubscribe:
    case DeviceEventType::kCommissioningSessionStarted:
        s_ble_subscribed = true;
        s_ble_adv_cooldown_ms = 0;
        ESP_LOGI(TAG, "Active pairing session started -> Fast pulsing indicator");
        lamp_set_pairing_state(LAMP_PAIRING_ACTIVE);
        break;

    case DeviceEventType::kCommissioningSessionStopped:
    case DeviceEventType::kFailSafeTimerExpired:
        if (!is_device_commissioned()) {
            s_ble_adv_cooldown_ms = 1200;
            (void)chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false);
            ESP_LOGI(TAG, "Commissioning session / fail-safe ended while uncommissioned -> Cooldown & Slow breathing");
            lamp_set_pairing_state(LAMP_PAIRING_WAITING);
        } else {
            lamp_set_pairing_state(LAMP_PAIRING_NONE);
        }
        break;

    case DeviceEventType::kCHIPoBLEUnsubscribe:
    case DeviceEventType::kCHIPoBLEConnectionClosed:
        s_ble_subscribed = false;
        if (!is_device_commissioned()) {
            s_ble_adv_cooldown_ms = 1200;
            (void)chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false);
            ESP_LOGI(TAG, "BLE connection closed while uncommissioned -> Cooldown & Returning to slow breathing");
            lamp_set_pairing_state(LAMP_PAIRING_WAITING);
            if (!chip::Server::GetInstance().GetFailSafeContext().IsFailSafeArmed() &&
                !chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen()) {
                (void)PlatformMgr().ScheduleWork(reopen_commissioning_window_work, 0);
            }
        } else {
            lamp_set_pairing_state(LAMP_PAIRING_NONE);
        }
        break;

    case DeviceEventType::kCommissioningComplete:
        s_ble_subscribed = false;
        s_ble_adv_cooldown_ms = 0;
        ESP_LOGI(TAG, "Matter commissioning completed successfully! Returning to normal light state.");
        lamp_set_pairing_state(LAMP_PAIRING_NONE);
        break;

    case DeviceEventType::kFabricRemoved:
        if (!is_device_commissioned() && lamp_get_pairing_state() != LAMP_PAIRING_FACTORY_RESET) {
            ESP_LOGW(TAG, "All Matter fabrics removed remotely -> Flashing confirmation and factory resetting...");
            lamp_set_pairing_state(LAMP_PAIRING_FACTORY_RESET);
            xTaskCreate(remote_factory_reset_task, "rem_rst", 3072, nullptr, 5, nullptr);
        }
        break;

    default:
        break;
    }
}

static void ble_watchdog_task(void *pvParameters)
{
    // Wait for Matter stack & BLE advertising to initialize after boot
    vTaskDelay(pdMS_TO_TICKS(4000));

    uint32_t unsubscribed_conn_ms = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(200));

        if (is_device_commissioned()) {
            // Once a fabric is added (or on commissioned boot), esp-matter deinitializes
            // NimBLE after commissioning completes. Exit the BLE watchdog task so
            // ble_gap_adv_active() is never called after NimBLE is deinitialized.
            vTaskDelete(nullptr);
            return;
        }

        // Post-disconnect cooldown: keep BLE advertising off for 1.2s to allow Android's
        // BluetoothGatt client to cleanly finish teardown before reconnecting.
        if (s_ble_adv_cooldown_ms > 0) {
            s_ble_adv_cooldown_ms -= 200;
            if (s_ble_adv_cooldown_ms <= 0) {
                s_ble_adv_cooldown_ms = 0;
                ESP_LOGI(TAG, "Post-disconnect BLE cooldown complete (1200 ms) -> resuming CHIPoBLE advertising");
                (void)chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
                    if (!is_device_commissioned()) {
                        auto &cwm = chip::Server::GetInstance().GetCommissioningWindowManager();
                        if (!cwm.IsCommissioningWindowOpen()) {
                            cwm.CloseCommissioningWindow();
                            (void)cwm.OpenBasicCommissioningWindow();
                        }
                        (void)chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(true);
                    }
                }, 0);
            }
            unsubscribed_conn_ms = 0;
            continue;
        }

        // If CHIPoBLE advertising is stopped while uncommissioned and no commissioner
        // has subscribed to the CHIPoBLE GATT characteristic for 5.0 seconds, either:
        //  1) A stale/zombie BLE GAP connection is wedging BLE advertising, or
        //  2) An earlier PASE session disconnected without arming the fail-safe.
        if (!ble_gap_adv_active() && !s_ble_subscribed) {
            unsubscribed_conn_ms += 200;
            if (unsubscribed_conn_ms >= 5000) {
                bool terminated_any = false;
                for (uint16_t handle = 0; handle < 4; ++handle) {
                    if (ble_gap_terminate(handle, 0x13) == 0) {
                        terminated_any = true;
                    }
                }
                if (terminated_any) {
                    ESP_LOGW(TAG, "Zombie unsubscribed BLE connection wedging CHIPoBLE advertising (%u ms) -> terminated GAP handle",
                             (unsigned)unsubscribed_conn_ms);
                } else {
                    ESP_LOGW(TAG, "BLE advertising inactive with no GAP connection (%u ms) -> reopening commissioning window",
                             (unsigned)unsubscribed_conn_ms);
                    (void)chip::DeviceLayer::PlatformMgr().ScheduleWork(reopen_commissioning_window_work, 0);
                }
                unsubscribed_conn_ms = 0;
            }
        } else {
            unsubscribed_conn_ms = 0;
        }
    }
}

static void ensure_wifi_sta_only_nvs()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("nvs.net80211", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return;
    }

    uint8_t opmode = 0;
    esp_err_t get_err = nvs_get_u8(handle, "opmode", &opmode);
    nvs_close(handle);

    if (get_err != ESP_OK || (opmode != WIFI_MODE_AP && opmode != WIFI_MODE_APSTA)) {
        return;
    }

    err = nvs_open("nvs.net80211", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open nvs.net80211 for opmode migration: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(handle, "opmode", static_cast<uint8_t>(WIFI_MODE_STA));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Migrated persisted Wi-Fi NVS opmode %u -> %u (WIFI_MODE_STA)",
                 (unsigned)opmode, (unsigned)WIFI_MODE_STA);
    } else {
        ESP_LOGE(TAG, "Failed to migrate Wi-Fi NVS opmode %u -> %u: %s",
                 (unsigned)opmode, (unsigned)WIFI_MODE_STA, esp_err_to_name(err));
    }
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    // Initialize or preserve RTC-backed /errors log buffer across warm/watchdog/OTA reboots
    if (s_rtc_err_store.magic != WEB_ERR_RTC_MAGIC ||
        s_rtc_err_store.len > WEB_ERR_BUFFER_SIZE ||
        s_rtc_err_store.total < s_rtc_err_store.len) {
        s_rtc_err_store.magic = WEB_ERR_RTC_MAGIC;
        s_rtc_err_store.total = 0;
        s_rtc_err_store.len = 0;
    } else if (s_rtc_err_store.len > 0) {
        const char *reboot_marker = "--- [Reboot] ---\n";
        for (const char *p = reboot_marker; *p != '\0'; ++p) {
            ring_buf_append_byte(s_rtc_err_store.buf, WEB_ERR_BUFFER_SIZE,
                                 &s_rtc_err_store.total, &s_rtc_err_store.len, *p);
        }
    }

    // Allocate 32 KB heap buffer and enable in-memory circular logging (/logs & /errors)
    s_web_log_buf = static_cast<char *>(heap_caps_malloc(WEB_LOG_BUFFER_SIZE, MALLOC_CAP_8BIT));
    esp_log_set_vprintf(web_log_vprintf);
    chip::Logging::SetLogRedirectCallback(chip_log_redirect_cb);

    // Mark current OTA slot valid so bootloader won't roll back after a successful OTA update
    esp_ota_mark_app_valid_cancel_rollback();

    // Read factory Wi-Fi STA MAC dynamically to populate Serial Number & UniqueId
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_serial_str, sizeof(s_serial_str), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "Starting Xiaomi Lamp 1S Matter over Wi-Fi Firmware (Dev)");
    ESP_LOGI(TAG, "Firmware Version: %s", app_desc ? app_desc->version : "unknown");
    ESP_LOGI(TAG, "Serial / MAC:     %s", s_serial_str);
    ESP_LOGI(TAG, "Pairing Sticker:  1475-062-2865 (MT:QDOA2KGB00SUJN33U00)");
    ESP_LOGI(TAG, "Hardware:         ESP32-S0WD / ESP-WROOM-32D (Unicore)");
    ESP_LOGI(TAG, "=================================================");

    // 1. Initialize NVS (required by Wi-Fi & Matter credentials)
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS truncated or updated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ensure_wifi_sta_only_nvs();

    // 2. Initialize Lamp Hardware Driver (LEDC PWM & Power Rail)
    ESP_ERROR_CHECK(lamp_driver_init());

    // 3. Create Matter Node with Xiaomi Device Info
    node::config_t node_config;
    snprintf(node_config.root_node.basic_information.node_label,
             sizeof(node_config.root_node.basic_information.node_label),
             "Xiaomi Desk Lamp 1S");
    snprintf(node_config.root_node.basic_information.unique_id,
             sizeof(node_config.root_node.basic_information.unique_id),
             "%s", s_serial_str);

    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    // Add supplementary Basic Information attributes to Root Node
    endpoint_t *root_endpoint = endpoint::get(node, 0);
    cluster_t *basic_info = cluster::get(root_endpoint, chip::app::Clusters::BasicInformation::Id);
    if (basic_info) {
        cluster::basic_information::attribute::create_serial_number(basic_info, s_serial_str, strlen(s_serial_str));
        cluster::basic_information::attribute::create_part_number(basic_info, (char *)"MJTD01SYL", 9);
        cluster::basic_information::attribute::create_product_url(basic_info, (char *)"https://www.mi.com", 18);
    }

    // 4. Create Color Temperature Light Endpoint initialized with restored state
    // Clear any stale esp_matter attribute NVS cache so our restored NVS state & Null StartUp policies take precedence
    attribute::erase_val_in_nvs(1, OnOff::Id, OnOff::Attributes::OnOff::Id);
    attribute::erase_val_in_nvs(1, OnOff::Id, OnOff::Attributes::StartUpOnOff::Id);
    attribute::erase_val_in_nvs(1, LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
    attribute::erase_val_in_nvs(1, LevelControl::Id, LevelControl::Attributes::OnLevel::Id);
    attribute::erase_val_in_nvs(1, LevelControl::Id, LevelControl::Attributes::StartUpCurrentLevel::Id);
    attribute::erase_val_in_nvs(1, ColorControl::Id, ColorControl::Attributes::ColorTemperatureMireds::Id);
    attribute::erase_val_in_nvs(1, ColorControl::Id, ColorControl::Attributes::StartUpColorTemperatureMireds::Id);

    color_temperature_light::config_t light_config;
    light_config.on_off.on_off = lamp_get_power();
    light_config.on_off_lighting.start_up_on_off = nullptr; // Null = preserve previous OnOff state on reboot
    light_config.level_control.current_level = lamp_get_brightness();
    light_config.level_control.on_level = nullptr;          // Null = use CurrentLevel when turning on
    light_config.level_control_lighting.start_up_current_level = nullptr; // Null = preserve previous level on reboot
    light_config.color_control_color_temperature.color_temperature_mireds = lamp_get_color_temp();
    light_config.color_control_color_temperature.start_up_color_temperature_mireds = nullptr; // Null = preserve mireds
    light_config.color_control_color_temperature.color_temp_physical_min_mireds = LAMP_MIN_MIREDS;
    light_config.color_control_color_temperature.color_temp_physical_max_mireds = LAMP_MAX_MIREDS;

    endpoint_t *endpoint = color_temperature_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create color temperature light endpoint");
        return;
    }

    uint16_t endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Color Temperature Light created at Endpoint ID: %u", endpoint_id);

    // 5. Initialize Physical Controls (Push Button + Rotary Knob)
    ESP_ERROR_CHECK(knob_driver_init(endpoint_id));

    // 6. Configure Commissioning Data Provider (3497-011-2332 / MT:Y.K9042C00KA0648G00)
    esp_matter::set_custom_commissionable_data_provider(&s_lamp_commissionable_data);

    // 7. Start Matter Stack (Local Wi-Fi OTA Server starts automatically upon IP_EVENT_STA_GOT_IP)
    ESP_ERROR_CHECK(esp_matter::start(app_event_cb));
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, nullptr);
    xTaskCreate(ble_watchdog_task, "ble_wdg", 2560, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "Matter stack started (240 MHz). Ready for commissioning!");
}
