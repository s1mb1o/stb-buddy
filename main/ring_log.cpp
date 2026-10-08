#include "ring_log.hpp"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"

namespace stb_buddy {

namespace {

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

}  // namespace

esp_err_t RingLog::init(const std::size_t capacity) {
    if (capacity == 0 || data_ != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    data_ = static_cast<std::uint8_t*>(
        heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (data_ == nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    capacity_ = capacity;
    return ESP_OK;
}

void RingLog::copy_in(const std::uint64_t offset, const std::uint8_t* data,
                      const std::size_t length) {
    const auto index = static_cast<std::size_t>(offset % capacity_);
    const auto first = std::min(length, capacity_ - index);
    std::memcpy(data_ + index, data, first);
    if (first < length) {
        std::memcpy(data_, data + first, length - first);
    }
}

void RingLog::copy_out(const std::uint64_t offset, std::uint8_t* output,
                       const std::size_t length) const {
    const auto index = static_cast<std::size_t>(offset % capacity_);
    const auto first = std::min(length, capacity_ - index);
    std::memcpy(output, data_ + index, first);
    if (first < length) {
        std::memcpy(output + first, data_, length - first);
    }
}

void RingLog::append(const std::uint8_t* data, std::size_t length) {
    if (data == nullptr || length == 0 || data_ == nullptr) {
        return;
    }

    Lock lock(mutex_);
    const auto original_length = length;
    if (length >= capacity_) {
        data += length - capacity_;
        length = capacity_;
        start_ = end_ + original_length - capacity_;
    } else {
        const auto retained = static_cast<std::size_t>(end_ - start_);
        if (retained + length > capacity_) {
            start_ += retained + length - capacity_;
        }
    }

    copy_in(end_ + original_length - length, data, length);
    end_ += original_length;
}

std::size_t RingLog::read(std::uint64_t since, std::uint8_t* output,
                          const std::size_t maximum,
                          std::uint64_t* actual_start) {
    if (output == nullptr || actual_start == nullptr || maximum == 0 ||
        data_ == nullptr) {
        return 0;
    }

    Lock lock(mutex_);
    since = std::max(since, start_);
    since = std::min(since, end_);
    *actual_start = since;
    const auto available = static_cast<std::size_t>(end_ - since);
    const auto length = std::min(available, maximum);
    copy_out(since, output, length);
    return length;
}

RingLogSnapshot RingLog::snapshot() {
    Lock lock(mutex_);
    return RingLogSnapshot{
        .start = start_,
        .end = end_,
        .size = static_cast<std::size_t>(end_ - start_),
        .capacity = capacity_,
    };
}

std::uint64_t RingLog::clear() {
    Lock lock(mutex_);
    start_ = end_;
    return end_;
}

}  // namespace stb_buddy

