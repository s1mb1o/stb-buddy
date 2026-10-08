#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace stb_buddy {

struct RingLogSnapshot {
    std::uint64_t start;
    std::uint64_t end;
    std::size_t size;
    std::size_t capacity;
};

class RingLog final {
public:
    RingLog() = default;
    RingLog(const RingLog&) = delete;
    RingLog& operator=(const RingLog&) = delete;

    esp_err_t init(std::size_t capacity);
    void append(const std::uint8_t* data, std::size_t length);
    std::size_t read(std::uint64_t since, std::uint8_t* output,
                     std::size_t maximum, std::uint64_t* actual_start);
    RingLogSnapshot snapshot();
    std::uint64_t clear();

private:
    void copy_in(std::uint64_t offset, const std::uint8_t* data,
                 std::size_t length);
    void copy_out(std::uint64_t offset, std::uint8_t* output,
                  std::size_t length) const;

    std::uint8_t* data_{nullptr};
    std::size_t capacity_{0};
    std::uint64_t start_{0};
    std::uint64_t end_{0};
    SemaphoreHandle_t mutex_{nullptr};
};

}  // namespace stb_buddy

