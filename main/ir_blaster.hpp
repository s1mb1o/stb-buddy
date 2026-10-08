#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "driver/rmt_types.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ir_waveform.hpp"
#include "lircd.hpp"

namespace stb_buddy {

constexpr std::size_t kIrEventCapacity = 32;
constexpr std::size_t kIrEventMessageBytes = 128;

struct IrEvent {
    std::uint64_t sequence{0};
    std::int64_t uptime_ms{0};
    std::array<char, kIrEventMessageBytes> message{};
};

struct IrEventBatch {
    std::array<IrEvent, kIrEventCapacity> events{};
    std::size_t count{0};
    std::uint64_t oldest{0};
    std::uint64_t next{0};
};

class IrBlaster final {
public:
    IrBlaster() = default;
    IrBlaster(const IrBlaster&) = delete;
    IrBlaster& operator=(const IrBlaster&) = delete;

    esp_err_t init();
    esp_err_t send(const LircdRemote& remote, std::uint64_t code,
                   unsigned repeats, const char* key_name = nullptr);
    esp_err_t send_nec(std::uint16_t address, std::uint8_t command,
                       bool extended_address, unsigned repeats);
    esp_err_t send_raw(std::uint32_t carrier_hz, std::uint8_t duty_percent,
                       const std::uint32_t* durations_us,
                       std::size_t duration_count, std::uint32_t gap_us,
                       unsigned repeats);
    bool events_since(std::uint64_t since, IrEventBatch* output);
    bool ready() const { return channel_ != nullptr; }

private:
    esp_err_t transmit_frame(const LircdRemote& remote, std::uint64_t code,
                             bool repeat_frame);
    esp_err_t transmit_waveform(std::uint32_t carrier_hz,
                                std::uint8_t duty_percent,
                                const IrWaveform& waveform);
    void record_event(const char* format, ...);
    void wait_until_next_frame();

    rmt_channel_handle_t channel_{nullptr};
    rmt_encoder_handle_t encoder_{nullptr};
    SemaphoreHandle_t mutex_{nullptr};
    SemaphoreHandle_t event_mutex_{nullptr};
    std::int64_t next_frame_at_us_{0};
    std::array<IrEvent, kIrEventCapacity> events_{};
    std::size_t event_count_{0};
    std::uint64_t next_event_sequence_{1};
};

}  // namespace stb_buddy
