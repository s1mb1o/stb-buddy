#include "ir_blaster.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ir_waveform.hpp"

namespace stb_buddy {

namespace {

constexpr auto kTag = "ir";
constexpr gpio_num_t kIrPin = GPIO_NUM_47;
constexpr std::uint32_t kResolutionHz = 1'000'000;

class Lock final {
public:
    explicit Lock(const SemaphoreHandle_t mutex) : mutex_(mutex) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
    }
    ~Lock() { xSemaphoreGive(mutex_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    SemaphoreHandle_t mutex_;
};

}  // namespace

esp_err_t IrBlaster::init() {
    if (channel_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    rmt_tx_channel_config_t config{};
    config.gpio_num = kIrPin;
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = kResolutionHz;
    config.mem_block_symbols = 64;
    config.trans_queue_depth = 4;
    config.intr_priority = 0;
    config.flags.invert_out = false;
    config.flags.with_dma = false;
    config.flags.io_loop_back = false;
    config.flags.io_od_mode = false;

    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&config, &channel_), kTag,
                        "create RMT TX channel");
    rmt_copy_encoder_config_t encoder_config{};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&encoder_config, &encoder_), kTag,
                        "create RMT copy encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(channel_), kTag,
                        "enable RMT TX channel");
    mutex_ = xSemaphoreCreateMutex();
    event_mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr || event_mutex_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "IR RMT channel ready on GPIO47");
    return ESP_OK;
}

void IrBlaster::wait_until_next_frame() {
    while (true) {
        const auto remaining = next_frame_at_us_ - esp_timer_get_time();
        if (remaining <= 0) {
            return;
        }
        if (remaining > 2000) {
            const auto sleep_ms = static_cast<TickType_t>((remaining - 1000) /
                                                           1000);
            vTaskDelay(pdMS_TO_TICKS(sleep_ms));
        } else {
            esp_rom_delay_us(static_cast<std::uint32_t>(remaining));
        }
    }
}

esp_err_t IrBlaster::transmit_frame(const LircdRemote& remote,
                                    const std::uint64_t code,
                                    const bool repeat_frame) {
    IrWaveform waveform{};
    const char* waveform_error = nullptr;
    if (!IrWaveformBuilder::build(remote, code, repeat_frame, &waveform,
                                  &waveform_error)) {
        ESP_LOGE(kTag, "cannot build waveform: %s", waveform_error);
        return ESP_ERR_INVALID_ARG;
    }

    return transmit_waveform(remote.frequency, remote.duty_cycle, waveform);
}

esp_err_t IrBlaster::transmit_waveform(const std::uint32_t carrier_hz,
                                       const std::uint8_t duty_percent,
                                       const IrWaveform& waveform) {
    if (waveform.count == 0 || waveform.count > kIrMaxDurations ||
        carrier_hz < kLircdMinimumCarrierHz ||
        carrier_hz > kLircdMaximumCarrierHz || duty_percent == 0 ||
        duty_percent > kLircdMaximumDutyPercent) {
        return ESP_ERR_INVALID_ARG;
    }

    std::array<rmt_symbol_word_t, (kIrMaxDurations + 1) / 2> symbols{};
    const auto symbol_count = (waveform.count + 1) / 2;
    for (std::size_t index = 0; index < symbol_count; ++index) {
        auto& symbol = symbols[index];
        const auto mark = waveform.durations[index * 2];
        std::uint32_t space = 0;
        if (index * 2 + 1 < waveform.count) {
            space = waveform.durations[index * 2 + 1];
        }
        symbol.val = mark | (std::uint32_t{1} << 15) | (space << 16);
    }

    rmt_carrier_config_t carrier{};
    carrier.frequency_hz = carrier_hz;
    carrier.duty_cycle = static_cast<float>(duty_percent) / 100.0F;
    carrier.flags.polarity_active_low = false;
    carrier.flags.always_on = false;
    ESP_RETURN_ON_ERROR(rmt_apply_carrier(channel_, &carrier), kTag,
                        "configure IR carrier");

    rmt_transmit_config_t transmit_config{};
    transmit_config.loop_count = 0;
    transmit_config.flags.eot_level = 0;
    transmit_config.flags.queue_nonblocking = false;
    wait_until_next_frame();
    ESP_RETURN_ON_ERROR(
        rmt_transmit(channel_, encoder_, symbols.data(),
                     symbol_count * sizeof(rmt_symbol_word_t),
                     &transmit_config),
        kTag, "transmit IR frame");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(channel_, pdMS_TO_TICKS(2000)),
                        kTag, "wait for IR frame");
    next_frame_at_us_ = esp_timer_get_time() + waveform.gap_us;
    return ESP_OK;
}

esp_err_t IrBlaster::send(const LircdRemote& remote, const std::uint64_t code,
                          const unsigned repeats, const char* key_name) {
    if (channel_ == nullptr || encoder_ == nullptr || mutex_ == nullptr ||
        repeats > 10) {
        return ESP_ERR_INVALID_ARG;
    }
    Lock lock(mutex_);
    ESP_RETURN_ON_ERROR(transmit_frame(remote, code, false), kTag,
                        "send initial IR frame");
    for (unsigned repeat = 0; repeat < repeats; ++repeat) {
        ESP_RETURN_ON_ERROR(transmit_frame(remote, code, true), kTag,
                            "send repeated IR frame");
    }
    if (key_name != nullptr && key_name[0] != '\0') {
        record_event("key %s/%s sent, repeats=%u", remote.name.data(),
                     key_name, repeats);
    } else {
        record_event("key %s/0x%llx sent, repeats=%u", remote.name.data(),
                     static_cast<unsigned long long>(code), repeats);
    }
    return ESP_OK;
}

esp_err_t IrBlaster::send_raw(const std::uint32_t carrier_hz,
                              const std::uint8_t duty_percent,
                              const std::uint32_t* durations_us,
                              const std::size_t duration_count,
                              const std::uint32_t gap_us,
                              const unsigned repeats) {
    if (channel_ == nullptr || encoder_ == nullptr || mutex_ == nullptr ||
        durations_us == nullptr || duration_count == 0 ||
        duration_count > kIrMaxDurations || repeats > 10 ||
        carrier_hz < kLircdMinimumCarrierHz ||
        carrier_hz > kLircdMaximumCarrierHz || duty_percent == 0 ||
        duty_percent > kLircdMaximumDutyPercent ||
        gap_us > kLircdMaximumGapUs) {
        return ESP_ERR_INVALID_ARG;
    }

    IrWaveform waveform{};
    waveform.count = duration_count;
    waveform.gap_us = gap_us;
    for (std::size_t index = 0; index < duration_count; ++index) {
        const auto duration = durations_us[index];
        if (duration == 0 || duration > 32767 ||
            waveform.signal_us > 1'000'000 - duration) {
            return ESP_ERR_INVALID_ARG;
        }
        waveform.durations[index] = duration;
        waveform.signal_us += duration;
    }

    Lock lock(mutex_);
    for (unsigned frame = 0; frame <= repeats; ++frame) {
        ESP_RETURN_ON_ERROR(
            transmit_waveform(carrier_hz, duty_percent, waveform), kTag,
            "send raw IR frame");
    }
    record_event("raw %lu Hz signal sent, durations=%u, repeats=%u",
                 static_cast<unsigned long>(carrier_hz),
                 static_cast<unsigned>(duration_count), repeats);
    return ESP_OK;
}

esp_err_t IrBlaster::send_nec(const std::uint16_t address,
                              const std::uint8_t command,
                              const bool extended_address,
                              const unsigned repeats) {
    if ((!extended_address && address > 0xff) || repeats > 10) {
        return ESP_ERR_INVALID_ARG;
    }

    std::array<std::uint32_t, kIrMaxDurations> initial{};
    std::size_t count = 0;
    initial[count++] = 9000;
    initial[count++] = 4500;
    const std::array<std::uint8_t, 4> bytes{
        static_cast<std::uint8_t>(address & 0xff),
        extended_address
            ? static_cast<std::uint8_t>((address >> 8) & 0xff)
            : static_cast<std::uint8_t>(~address & 0xff),
        command,
        static_cast<std::uint8_t>(~command),
    };
    for (const auto byte : bytes) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            initial[count++] = 560;
            initial[count++] = ((byte >> bit) & 1U) != 0 ? 1690 : 560;
        }
    }
    initial[count++] = 560;

    Lock lock(mutex_);
    IrWaveform waveform{};
    waveform.count = count;
    for (std::size_t index = 0; index < count; ++index) {
        waveform.durations[index] = initial[index];
        waveform.signal_us += initial[index];
    }
    waveform.gap_us = 110000 - waveform.signal_us;
    ESP_RETURN_ON_ERROR(transmit_waveform(38000, 33, waveform), kTag,
                        "send NEC frame");

    IrWaveform repeat{};
    repeat.durations[0] = 9000;
    repeat.durations[1] = 2250;
    repeat.durations[2] = 560;
    repeat.count = 3;
    repeat.signal_us = 11810;
    repeat.gap_us = 110000 - repeat.signal_us;
    for (unsigned index = 0; index < repeats; ++index) {
        ESP_RETURN_ON_ERROR(transmit_waveform(38000, 33, repeat), kTag,
                            "send NEC repeat");
    }
    record_event("NEC address=0x%04x command=0x%02x sent, repeats=%u",
                 static_cast<unsigned>(address),
                 static_cast<unsigned>(command), repeats);
    return ESP_OK;
}

void IrBlaster::record_event(const char* format, ...) {
    if (event_mutex_ == nullptr || format == nullptr) {
        return;
    }
    Lock lock(event_mutex_);
    const auto event_index = static_cast<std::size_t>(
        (next_event_sequence_ - 1) % events_.size());
    auto& event = events_[event_index];
    event = IrEvent{};
    event.sequence = next_event_sequence_++;
    event.uptime_ms = esp_timer_get_time() / 1000;
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(event.message.data(), event.message.size(), format,
                   arguments);
    va_end(arguments);
    event_count_ = std::min(event_count_ + 1, events_.size());
    ESP_LOGI(kTag, "%s", event.message.data());
}

bool IrBlaster::events_since(const std::uint64_t since,
                             IrEventBatch* output) {
    if (event_mutex_ == nullptr || output == nullptr) {
        return false;
    }
    Lock lock(event_mutex_);
    *output = IrEventBatch{};
    if (event_count_ == 0) {
        return true;
    }
    const auto oldest = next_event_sequence_ - event_count_;
    const auto latest = next_event_sequence_ - 1;
    output->oldest = oldest;
    output->next = latest;
    if (since >= latest) {
        return true;
    }
    const auto first = std::max(oldest, since + 1);
    for (auto sequence = first; sequence <= latest; ++sequence) {
        const auto event_index = static_cast<std::size_t>(
            (sequence - 1) % events_.size());
        output->events[output->count++] = events_[event_index];
    }
    return true;
}

}  // namespace stb_buddy
