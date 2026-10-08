#include "display_service.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace stb_buddy {

namespace {

constexpr auto kTag = "display";
constexpr gpio_num_t kButtonPin = GPIO_NUM_41;
constexpr std::uint32_t kLongPressMs = 2000;
constexpr std::uint32_t kDebounceMs = 30;
constexpr std::uint32_t kFrameMs = 100;
constexpr std::uint16_t kBackground = 0x0861;
constexpr std::uint16_t kPanel = 0x10E3;
constexpr std::uint16_t kText = 0xFFFF;
constexpr std::uint16_t kMuted = 0x9CF3;
constexpr std::uint16_t kGreen = 0x4E69;
constexpr std::uint16_t kYellow = 0xFDC0;
constexpr std::uint16_t kBlue = 0x2D7F;

std::uint64_t milliseconds() {
    return static_cast<std::uint64_t>(esp_timer_get_time()) / 1000;
}

}  // namespace

DisplayService::DisplayService() : canvas_(&display_) {}

esp_err_t DisplayService::start(WifiService* wifi) {
    if (wifi == nullptr || wifi_ != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display_.begin()) {
        ESP_LOGE(kTag, "AtomS3R display initialization failed");
        return ESP_FAIL;
    }
    display_.setBrightness(128);
    display_.setRotation(0);
    canvas_.setColorDepth(16);
    canvas_.setPsram(true);
    if (canvas_.createSprite(128, 128) == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t button{};
    button.pin_bit_mask = 1ULL << kButtonPin;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    button.pull_down_en = GPIO_PULLDOWN_DISABLE;
    button.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&button), kTag, "configure button");

    wifi_ = wifi;
    if (xTaskCreate(task_entry, "buddy_display", 6144, this, 5, nullptr) !=
        pdPASS) {
        wifi_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "128x128 display and GPIO41 setup button ready");
    return ESP_OK;
}

void DisplayService::task_entry(void* context) {
    static_cast<DisplayService*>(context)->task_loop();
}

void DisplayService::task_loop() {
    bool raw_pressed = false;
    bool stable_pressed = false;
    bool action_sent = false;
    auto raw_changed_ms = milliseconds();
    std::uint64_t pressed_ms = 0;

    while (true) {
        const auto now = milliseconds();
        const bool pressed = gpio_get_level(kButtonPin) == 0;
        if (pressed != raw_pressed) {
            raw_pressed = pressed;
            raw_changed_ms = now;
        }
        if (pressed != stable_pressed && now - raw_changed_ms >= kDebounceMs) {
            stable_pressed = pressed;
            if (stable_pressed) {
                pressed_ms = now;
                action_sent = false;
            } else {
                if (!action_sent && pressed_ms != 0) {
                    cycle_screen();
                }
                pressed_ms = 0;
                action_sent = false;
            }
        }

        std::uint32_t held_ms = 0;
        if (stable_pressed) {
            held_ms = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(now - pressed_ms, kLongPressMs));
            if (!action_sent && held_ms >= kLongPressMs) {
                action_sent = true;
                screen_ = Screen::main;
                const auto result = wifi_->enter_setup_mode();
                if (result != ESP_OK) {
                    ESP_LOGE(kTag, "enter Wi-Fi setup failed: %s",
                             esp_err_to_name(result));
                }
            }
        }
        render(now, held_ms);
        vTaskDelay(pdMS_TO_TICKS(kFrameMs));
    }
}

void DisplayService::cycle_screen() {
    const auto wifi = wifi_->snapshot();
    if (!wifi.setup_mode) {
        screen_ = wifi.connected && screen_ == Screen::main
                      ? Screen::wiring
                      : Screen::main;
        return;
    }
    switch (screen_) {
        case Screen::main:
            screen_ = Screen::wifi_qr;
            break;
        case Screen::wifi_qr:
            screen_ = Screen::page_qr;
            break;
        case Screen::page_qr:
            screen_ = Screen::main;
            break;
        case Screen::wiring:
            screen_ = Screen::main;
            break;
    }
}

void DisplayService::render(const std::uint64_t now_ms,
                            const std::uint32_t held_ms) {
    const auto wifi = wifi_->snapshot();
    if (!wifi.setup_mode && wifi.connected && screen_ == Screen::wiring) {
        render_wiring();
        return;
    }
    if (!wifi.setup_mode) {
        screen_ = Screen::main;
    } else if (screen_ == Screen::wifi_qr) {
        char payload[64]{};
        std::snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;",
                      wifi.ssid.data());
        render_qr("JOIN SETUP WIFI", payload);
        return;
    } else if (screen_ == Screen::page_qr) {
        render_qr("OPEN SETUP PAGE", "http://192.168.4.1/");
        return;
    }

    canvas_.fillSprite(kBackground);
    canvas_.fillRoundRect(4, 4, 120, 24, 6, kPanel);
    canvas_.setTextDatum(lgfx::textdatum::middle_center);
    canvas_.setTextSize(1);
    canvas_.setTextColor(kText, kPanel);
    canvas_.drawString("STB BUDDY", 64, 16);

    const char* state = "CONNECTING";
    auto state_color = kYellow;
    if (wifi.setup_mode) {
        state = "WIFI SETUP";
        state_color = kBlue;
    } else if (wifi.connected) {
        state = "ONLINE";
        state_color = kGreen;
    }
    canvas_.setTextColor(state_color, kBackground);
    canvas_.drawString(state, 64, 36);

    canvas_.setTextDatum(lgfx::textdatum::top_left);
    canvas_.setTextColor(kMuted, kBackground);
    canvas_.drawString("SSID", 8, 46);
    draw_marquee(wifi.ssid.data(), 58, now_ms);

    canvas_.setTextDatum(lgfx::textdatum::top_left);
    canvas_.setTextColor(kMuted, kBackground);
    canvas_.drawString("IP ADDRESS", 8, 78);
    canvas_.setTextColor(kText, kBackground);
    canvas_.setTextDatum(lgfx::textdatum::top_center);
    canvas_.drawString(wifi.ip[0] == '\0' ? "--" : wifi.ip.data(), 64, 90);

    canvas_.setTextDatum(lgfx::textdatum::top_center);
    canvas_.setTextColor(kMuted, kBackground);
    canvas_.drawString(
        wifi.setup_mode ? "PRESS: QR  WEB :80" : "PRESS:PINS HOLD:SETUP", 64,
        109);
    if (held_ms > 0 && !wifi.setup_mode) {
        canvas_.drawRoundRect(8, 121, 112, 4, 2, kMuted);
        const auto width = static_cast<std::int32_t>(
            (held_ms * 108U) / kLongPressMs);
        canvas_.fillRoundRect(10, 122, width, 2, 1, kBlue);
    }
    canvas_.pushSprite(0, 0);
}

void DisplayService::render_wiring() {
    canvas_.fillSprite(kBackground);
    canvas_.fillRoundRect(4, 4, 120, 22, 6, kPanel);
    canvas_.setTextDatum(lgfx::textdatum::middle_center);
    canvas_.setTextSize(1);
    canvas_.setTextColor(kText, kPanel);
    canvas_.drawString("UART CONNECTION", 64, 15);

    canvas_.setTextDatum(lgfx::textdatum::top_center);
    canvas_.setTextColor(kMuted, kBackground);
    canvas_.drawString("ATOM S3R       STB", 64, 30);
    canvas_.setTextColor(kText, kBackground);
    canvas_.drawString("GND    <-->    GND", 64, 44);
    canvas_.drawString("G2 TXD  -->   RXD", 64, 58);
    canvas_.drawString("G1 RXD  <--   TXD", 64, 72);

    canvas_.setTextColor(kYellow, kBackground);
    canvas_.drawString("3.3V UART ONLY", 64, 90);
    canvas_.drawString("USB: LEAVE 5V OPEN", 64, 103);
    canvas_.setTextColor(kMuted, kBackground);
    canvas_.drawString("PRESS: BACK", 64, 117);
    canvas_.pushSprite(0, 0);
}

void DisplayService::render_qr(const char* title, const char* payload) {
    canvas_.fillSprite(TFT_BLACK);
    canvas_.setTextDatum(lgfx::textdatum::top_center);
    canvas_.setTextSize(1);
    canvas_.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas_.drawString(title, 64, 1);
    canvas_.qrcode(payload, 8, 10, 112, 3, true);
    canvas_.pushSprite(0, 0);
}

void DisplayService::draw_marquee(const char* text, const std::int32_t y,
                                  const std::uint64_t now_ms) {
    canvas_.setTextColor(kText, kBackground);
    canvas_.setTextDatum(lgfx::textdatum::top_left);
    const auto width = canvas_.textWidth(text);
    constexpr std::int32_t kViewportX = 8;
    constexpr std::int32_t kViewportWidth = 112;
    if (width <= kViewportWidth) {
        canvas_.setTextDatum(lgfx::textdatum::top_center);
        canvas_.drawString(text, 64, y);
        return;
    }

    constexpr std::int32_t kGap = 24;
    const auto cycle = width + kGap;
    const auto offset = static_cast<std::int32_t>((now_ms / 40) % cycle);
    canvas_.setClipRect(kViewportX, y, kViewportWidth, 14);
    canvas_.drawString(text, kViewportX - offset, y);
    canvas_.drawString(text, kViewportX - offset + cycle, y);
    canvas_.clearClipRect();
}

}  // namespace stb_buddy
