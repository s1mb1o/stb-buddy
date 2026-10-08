#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace stb_buddy {

struct WifiSnapshot {
    bool setup_mode;
    bool connected;
    std::array<char, 33> ssid;
    std::array<char, 16> ip;
};

class WifiService final {
public:
    WifiService() = default;
    WifiService(const WifiService&) = delete;
    WifiService& operator=(const WifiService&) = delete;

    esp_err_t start();
    esp_err_t enter_setup_mode();
    esp_err_t save_credentials(const char* ssid, const char* password);
    WifiSnapshot snapshot() const;
    bool setup_mode() const;

private:
    static void event_entry(void* context, esp_event_base_t event_base,
                            std::int32_t event_id, void* event_data);
    void handle_event(esp_event_base_t event_base, std::int32_t event_id,
                      void* event_data);
    esp_err_t load_credentials(bool* found);
    void build_setup_ssid();
    void set_snapshot(bool setup_mode, bool connected, const char* ssid,
                      const char* ip);

    mutable SemaphoreHandle_t mutex_{nullptr};
    std::array<char, 33> station_ssid_{};
    std::array<char, 65> station_password_{};
    std::array<char, 33> setup_ssid_{};
    std::array<char, 33> active_ssid_{};
    std::array<char, 16> ip_{};
    bool setup_mode_{false};
    bool connected_{false};
    bool started_{false};
    esp_event_handler_instance_t wifi_events_{nullptr};
    esp_event_handler_instance_t ip_events_{nullptr};
};

}  // namespace stb_buddy
