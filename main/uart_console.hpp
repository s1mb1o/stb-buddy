#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ring_log.hpp"

namespace stb_buddy {

enum class UartParity : std::uint8_t {
    none,
    even,
    odd,
};

struct UartRuntimeConfig {
    std::uint32_t baud;
    std::uint8_t data_bits;
    UartParity parity;
    std::uint8_t stop_bits_x2;
};

constexpr std::size_t kUartNoteCapacity = 32;
constexpr std::size_t kUartNoteSourceBytes = 24;
constexpr std::size_t kUartNoteMessageBytes = 256;

struct UartNote {
    std::uint64_t sequence{0};
    std::int64_t uptime_ms{0};
    std::array<char, kUartNoteSourceBytes> source{};
    std::array<char, kUartNoteMessageBytes> message{};
};

struct UartNoteBatch {
    std::array<UartNote, kUartNoteCapacity> events{};
    std::size_t count{0};
    std::uint64_t oldest{0};
    std::uint64_t next{0};
};

const char* uart_parity_name(UartParity parity);

class UartConsole final {
public:
    UartConsole() = default;
    UartConsole(const UartConsole&) = delete;
    UartConsole& operator=(const UartConsole&) = delete;

    esp_err_t init(RingLog* history);
    esp_err_t write(const std::uint8_t* data, std::size_t length,
                    std::size_t* written);
    esp_err_t configure(const UartRuntimeConfig& config);
    UartRuntimeConfig configuration();
    void record_note(const char* source, const std::uint8_t* data,
                     std::size_t length, bool appended_enter = false);
    bool notes_since(std::uint64_t since, UartNoteBatch* output);
    std::size_t clear_notes();
    void begin_transaction();
    void end_transaction();
    bool connected() const { return initialized_; }

private:
    static void reader_entry(void* context);
    void reader_loop();

    RingLog* history_{nullptr};
    SemaphoreHandle_t transaction_mutex_{nullptr};
    SemaphoreHandle_t note_mutex_{nullptr};
    UartRuntimeConfig configuration_{};
    std::array<UartNote, kUartNoteCapacity> notes_{};
    std::size_t note_count_{0};
    std::uint64_t next_note_sequence_{1};
    bool initialized_{false};
};

}  // namespace stb_buddy
