#pragma once

#include <cstdint>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include "M5GFX.h"
#pragma GCC diagnostic pop
#include "esp_err.h"
#include "wifi_service.hpp"

namespace stb_buddy {

class DisplayService final {
public:
    DisplayService();
    DisplayService(const DisplayService&) = delete;
    DisplayService& operator=(const DisplayService&) = delete;

    esp_err_t start(WifiService* wifi);

private:
    enum class Screen : std::uint8_t {
        main,
        wifi_qr,
        page_qr,
        wiring,
    };

    static void task_entry(void* context);
    void task_loop();
    void cycle_screen();
    void render(std::uint64_t now_ms, std::uint32_t held_ms);
    void render_qr(const char* title, const char* payload);
    void render_wiring();
    void draw_marquee(const char* text, std::int32_t y, std::uint64_t now_ms);

    M5GFX display_;
    M5Canvas canvas_;
    WifiService* wifi_{nullptr};
    Screen screen_{Screen::main};
};

}  // namespace stb_buddy
