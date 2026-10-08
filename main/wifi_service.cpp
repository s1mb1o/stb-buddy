#include "wifi_service.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace stb_buddy {

namespace {

constexpr auto kTag = "wifi";
constexpr auto kNvsNamespace = "stb_wifi";
constexpr auto kNvsSsid = "ssid";
constexpr auto kNvsPassword = "password";

class Lock final {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~Lock() { xSemaphoreGive(mutex_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    SemaphoreHandle_t mutex_;
};

void copy_string(char* destination, const std::size_t capacity,
                 const char* source) {
    if (capacity == 0) {
        return;
    }
    std::snprintf(destination, capacity, "%s", source == nullptr ? "" : source);
}

}  // namespace

esp_err_t WifiService::start() {
    if (started_) {
        return ESP_ERR_INVALID_STATE;
    }
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "initialize network stack");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), kTag,
                        "create default event loop");
    if (esp_netif_create_default_wifi_sta() == nullptr ||
        esp_netif_create_default_wifi_ap() == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), kTag, "initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), kTag,
                        "use RAM Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, event_entry, this,
                            &wifi_events_),
                        kTag, "register Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, event_entry, this,
                            &ip_events_),
                        kTag, "register IP events");

    build_setup_ssid();
    bool have_credentials = false;
    ESP_RETURN_ON_ERROR(load_credentials(&have_credentials), kTag,
                        "load Wi-Fi credentials");

    wifi_config_t config{};
    if (have_credentials) {
        std::memcpy(config.sta.ssid, station_ssid_.data(),
                    std::strlen(station_ssid_.data()));
        copy_string(reinterpret_cast<char*>(config.sta.password),
                    sizeof(config.sta.password), station_password_.data());
        config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag,
                            "set station mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), kTag,
                            "configure station");
        set_snapshot(false, false, station_ssid_.data(), "");
    } else {
        std::memcpy(config.ap.ssid, setup_ssid_.data(),
                    std::strlen(setup_ssid_.data()));
        config.ap.ssid_len = static_cast<std::uint8_t>(
            std::strlen(setup_ssid_.data()));
        config.ap.channel = 1;
        config.ap.authmode = WIFI_AUTH_OPEN;
        config.ap.ssid_hidden = 0;
        config.ap.max_connection = 4;
        config.ap.beacon_interval = 100;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), kTag,
                            "set setup AP mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), kTag,
                            "configure setup AP");
        set_snapshot(true, true, setup_ssid_.data(), "192.168.4.1");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "start Wi-Fi");
    started_ = true;
    if (have_credentials) {
        ESP_LOGI(kTag, "connecting to saved SSID %s", station_ssid_.data());
    } else {
        ESP_LOGI(kTag, "setup AP ready: SSID=%s address=192.168.4.1",
                 setup_ssid_.data());
    }
    return ESP_OK;
}

esp_err_t WifiService::enter_setup_mode() {
    if (!started_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (setup_mode()) {
        return ESP_OK;
    }

    wifi_config_t config{};
    std::memcpy(config.ap.ssid, setup_ssid_.data(),
                std::strlen(setup_ssid_.data()));
    config.ap.ssid_len =
        static_cast<std::uint8_t>(std::strlen(setup_ssid_.data()));
    config.ap.channel = 1;
    config.ap.authmode = WIFI_AUTH_OPEN;
    config.ap.ssid_hidden = 0;
    config.ap.max_connection = 4;
    config.ap.beacon_interval = 100;

    const auto disconnect_result = esp_wifi_disconnect();
    if (disconnect_result != ESP_OK &&
        disconnect_result != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(kTag, "station disconnect before setup failed: %s",
                 esp_err_to_name(disconnect_result));
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), kTag,
                        "switch to setup AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), kTag,
                        "configure setup AP");
    set_snapshot(true, true, setup_ssid_.data(), "192.168.4.1");
    ESP_LOGI(kTag, "button requested setup AP: SSID=%s address=192.168.4.1",
             setup_ssid_.data());
    return ESP_OK;
}

esp_err_t WifiService::save_credentials(const char* ssid,
                                        const char* password) {
    if (ssid == nullptr || password == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    const auto ssid_length = std::strlen(ssid);
    const auto password_length = std::strlen(password);
    if (ssid_length == 0 || ssid_length > 32 || password_length > 63 ||
        (password_length != 0 && password_length < 8)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &handle), kTag,
                        "open Wi-Fi NVS");
    auto result = nvs_set_str(handle, kNvsSsid, ssid);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, kNvsPassword, password);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

WifiSnapshot WifiService::snapshot() const {
    WifiSnapshot result{};
    Lock lock(mutex_);
    result.setup_mode = setup_mode_;
    result.connected = connected_;
    result.ssid = active_ssid_;
    result.ip = ip_;
    return result;
}

bool WifiService::setup_mode() const {
    Lock lock(mutex_);
    return setup_mode_;
}

void WifiService::event_entry(void* context, const esp_event_base_t event_base,
                              const std::int32_t event_id, void* event_data) {
    static_cast<WifiService*>(context)->handle_event(event_base, event_id,
                                                     event_data);
}

void WifiService::handle_event(const esp_event_base_t event_base,
                               const std::int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!setup_mode()) {
            set_snapshot(false, false, station_ssid_.data(), "");
            const auto result = esp_wifi_connect();
            if (result != ESP_OK) {
                ESP_LOGW(kTag, "Wi-Fi reconnect failed: %s",
                         esp_err_to_name(result));
            }
        }
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        char ip[16]{};
        std::snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        set_snapshot(false, true, station_ssid_.data(), ip);
        ESP_LOGI(kTag, "station connected: SSID=%s address=%s",
                 station_ssid_.data(), ip);
    }
}

esp_err_t WifiService::load_credentials(bool* found) {
    if (found == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *found = false;
    nvs_handle_t handle = 0;
    auto result = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "open Wi-Fi NVS");

    std::size_t ssid_size = station_ssid_.size();
    result = nvs_get_str(handle, kNvsSsid, station_ssid_.data(), &ssid_size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (result != ESP_OK || ssid_size <= 1 || ssid_size > station_ssid_.size()) {
        nvs_close(handle);
        return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
    }

    std::size_t password_size = station_password_.size();
    result = nvs_get_str(handle, kNvsPassword, station_password_.data(),
                         &password_size);
    nvs_close(handle);
    if (result != ESP_OK || password_size == 0 ||
        password_size > station_password_.size()) {
        return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
    }
    *found = true;
    return ESP_OK;
}

void WifiService::build_setup_ssid() {
    std::uint8_t mac[6]{};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    std::snprintf(setup_ssid_.data(), setup_ssid_.size(),
                  "%s-%02X%02X%02X%02X%02X%02X",
                  CONFIG_STB_BUDDY_SETUP_AP_PREFIX, mac[0], mac[1], mac[2],
                  mac[3], mac[4], mac[5]);
}

void WifiService::set_snapshot(const bool setup_mode, const bool connected,
                               const char* ssid, const char* ip) {
    Lock lock(mutex_);
    setup_mode_ = setup_mode;
    connected_ = connected;
    copy_string(active_ssid_.data(), active_ssid_.size(), ssid);
    copy_string(ip_.data(), ip_.size(), ip);
}

}  // namespace stb_buddy
