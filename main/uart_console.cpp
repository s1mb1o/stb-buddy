#include "uart_console.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace stb_buddy {

namespace {

constexpr auto kTag = "uart";
constexpr uart_port_t kPort = UART_NUM_1;
constexpr gpio_num_t kTxPin = GPIO_NUM_2;
constexpr gpio_num_t kRxPin = GPIO_NUM_1;
constexpr std::size_t kDriverBufferBytes = 8192;

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

bool valid_configuration(const UartRuntimeConfig& config) {
    return config.baud >= 1200 && config.baud <= 3'000'000 &&
           config.data_bits >= 5 && config.data_bits <= 8 &&
           (config.stop_bits_x2 == 2 || config.stop_bits_x2 == 3 ||
            config.stop_bits_x2 == 4);
}

uart_word_length_t word_length(const std::uint8_t bits) {
    return static_cast<uart_word_length_t>(bits - 5);
}

uart_parity_t parity_value(const UartParity parity) {
    switch (parity) {
        case UartParity::even:
            return UART_PARITY_EVEN;
        case UartParity::odd:
            return UART_PARITY_ODD;
        case UartParity::none:
        default:
            return UART_PARITY_DISABLE;
    }
}

uart_stop_bits_t stop_bits_value(const std::uint8_t value) {
    switch (value) {
        case 3:
            return UART_STOP_BITS_1_5;
        case 4:
            return UART_STOP_BITS_2;
        case 2:
        default:
            return UART_STOP_BITS_1;
    }
}

}  // namespace

const char* uart_parity_name(const UartParity parity) {
    switch (parity) {
        case UartParity::even:
            return "even";
        case UartParity::odd:
            return "odd";
        case UartParity::none:
        default:
            return "none";
    }
}

esp_err_t UartConsole::init(RingLog* history) {
    if (history == nullptr || initialized_) {
        return ESP_ERR_INVALID_ARG;
    }

    transaction_mutex_ = xSemaphoreCreateRecursiveMutex();
    note_mutex_ = xSemaphoreCreateMutex();
    if (transaction_mutex_ == nullptr || note_mutex_ == nullptr) {
        if (transaction_mutex_ != nullptr) {
            vSemaphoreDelete(transaction_mutex_);
            transaction_mutex_ = nullptr;
        }
        if (note_mutex_ != nullptr) {
            vSemaphoreDelete(note_mutex_);
            note_mutex_ = nullptr;
        }
        return ESP_ERR_NO_MEM;
    }

    configuration_ = UartRuntimeConfig{
        .baud = CONFIG_STB_BUDDY_UART_BAUD,
        .data_bits = 8,
        .parity = UartParity::none,
        .stop_bits_x2 = 2,
    };

    uart_config_t config{};
    config.baud_rate = CONFIG_STB_BUDDY_UART_BAUD;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.rx_flow_ctrl_thresh = 0;
    config.source_clk = UART_SCLK_DEFAULT;

    ESP_RETURN_ON_ERROR(uart_driver_install(
                            kPort, kDriverBufferBytes, kDriverBufferBytes, 0,
                            nullptr, 0),
                        kTag, "install UART driver");
    ESP_RETURN_ON_ERROR(uart_param_config(kPort, &config), kTag,
                        "configure UART");
    ESP_RETURN_ON_ERROR(
        uart_set_pin(kPort, kTxPin, kRxPin, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE),
        kTag, "set UART pins");

    history_ = history;
    initialized_ = true;
    const auto created = xTaskCreate(reader_entry, "stb_uart_rx", 4096, this,
                                     12, nullptr);
    if (created != pdPASS) {
        initialized_ = false;
        history_ = nullptr;
        uart_driver_delete(kPort);
        vSemaphoreDelete(transaction_mutex_);
        transaction_mutex_ = nullptr;
        vSemaphoreDelete(note_mutex_);
        note_mutex_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "UART1 ready: RX=GPIO1 TX=GPIO2 baud=%d",
             CONFIG_STB_BUDDY_UART_BAUD);
    return ESP_OK;
}

esp_err_t UartConsole::write(const std::uint8_t* data,
                             const std::size_t length,
                             std::size_t* written) {
    if (!initialized_ || data == nullptr || written == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTakeRecursive(transaction_mutex_, portMAX_DELAY);
    const auto result = uart_write_bytes(kPort, data, length);
    xSemaphoreGiveRecursive(transaction_mutex_);
    if (result < 0) {
        return ESP_FAIL;
    }
    *written = static_cast<std::size_t>(result);
    return ESP_OK;
}

esp_err_t UartConsole::configure(const UartRuntimeConfig& config) {
    if (!initialized_ || !valid_configuration(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    begin_transaction();
    uart_config_t driver_config{};
    driver_config.baud_rate = static_cast<int>(config.baud);
    driver_config.data_bits = word_length(config.data_bits);
    driver_config.parity = parity_value(config.parity);
    driver_config.stop_bits = stop_bits_value(config.stop_bits_x2);
    driver_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    driver_config.rx_flow_ctrl_thresh = 0;
    driver_config.source_clk = UART_SCLK_DEFAULT;
    auto result = uart_wait_tx_done(kPort, pdMS_TO_TICKS(1000));
    if (result == ESP_OK) {
        result = uart_param_config(kPort, &driver_config);
    }
    if (result == ESP_OK) {
        configuration_ = config;
        ESP_LOGI(kTag, "UART1 reconfigured: %lu %u%s%g",
                 static_cast<unsigned long>(config.baud), config.data_bits,
                 uart_parity_name(config.parity),
                 static_cast<double>(config.stop_bits_x2) / 2.0);
    }
    end_transaction();
    return result;
}

UartRuntimeConfig UartConsole::configuration() {
    begin_transaction();
    const auto result = configuration_;
    end_transaction();
    return result;
}

void UartConsole::record_note(const char* source, const std::uint8_t* data,
                              const std::size_t length,
                              const bool appended_enter) {
    if (note_mutex_ == nullptr || source == nullptr || data == nullptr ||
        (length == 0 && !appended_enter)) {
        return;
    }

    UartNote note{};
    std::snprintf(note.source.data(), note.source.size(), "%s", source);
    std::size_t output = 0;
    bool truncated = false;
    auto append = [&](const char* text, const std::size_t count) {
        if (output + count >= note.message.size()) {
            truncated = true;
            return false;
        }
        std::memcpy(note.message.data() + output, text, count);
        output += count;
        return true;
    };

    if (length == 0 && appended_enter) {
        constexpr char kEnter[] = "<Enter>";
        append(kEnter, sizeof(kEnter) - 1);
    } else {
        constexpr char kHex[] = "0123456789ABCDEF";
        for (std::size_t index = 0; index < length; ++index) {
            const auto byte = data[index];
            char representation[4]{};
            std::size_t representation_length = 1;
            if (byte < 0x20) {
                representation[0] = '^';
                representation[1] = static_cast<char>(byte + 0x40);
                representation_length = 2;
            } else if (byte == 0x7f) {
                representation[0] = '^';
                representation[1] = '?';
                representation_length = 2;
            } else if (byte >= 0x80) {
                representation[0] = '\\';
                representation[1] = 'x';
                representation[2] = kHex[(byte >> 4) & 0x0f];
                representation[3] = kHex[byte & 0x0f];
                representation_length = 4;
            } else {
                representation[0] = static_cast<char>(byte);
            }
            if (!append(representation, representation_length)) {
                break;
            }
        }
    }
    if (truncated) {
        constexpr char kEllipsis[] = "...";
        const auto maximum_prefix = note.message.size() - sizeof(kEllipsis);
        output = std::min(output, maximum_prefix);
        std::memcpy(note.message.data() + output, kEllipsis,
                    sizeof(kEllipsis) - 1);
        output += sizeof(kEllipsis) - 1;
    }
    note.message[output] = '\0';
    note.uptime_ms = esp_timer_get_time() / 1000;

    Lock lock(note_mutex_);
    note.sequence = next_note_sequence_++;
    const auto note_index = static_cast<std::size_t>(
        (note.sequence - 1) % notes_.size());
    notes_[note_index] = note;
    note_count_ = std::min(note_count_ + 1, notes_.size());
}

bool UartConsole::notes_since(const std::uint64_t since,
                              UartNoteBatch* output) {
    if (note_mutex_ == nullptr || output == nullptr) {
        return false;
    }
    Lock lock(note_mutex_);
    *output = UartNoteBatch{};
    const auto latest = next_note_sequence_ - 1;
    output->next = latest;
    if (note_count_ == 0) {
        return true;
    }
    const auto oldest = next_note_sequence_ - note_count_;
    output->oldest = oldest;
    if (since >= latest) {
        return true;
    }
    const auto first = std::max(oldest, since + 1);
    for (auto sequence = first; sequence <= latest; ++sequence) {
        const auto note_index = static_cast<std::size_t>(
            (sequence - 1) % notes_.size());
        output->events[output->count++] = notes_[note_index];
    }
    return true;
}

std::size_t UartConsole::clear_notes() {
    if (note_mutex_ == nullptr) {
        return 0;
    }
    Lock lock(note_mutex_);
    const auto cleared = note_count_;
    note_count_ = 0;
    for (auto& note : notes_) {
        note = UartNote{};
    }
    return cleared;
}

void UartConsole::begin_transaction() {
    xSemaphoreTakeRecursive(transaction_mutex_, portMAX_DELAY);
}

void UartConsole::end_transaction() {
    xSemaphoreGiveRecursive(transaction_mutex_);
}

void UartConsole::reader_entry(void* context) {
    static_cast<UartConsole*>(context)->reader_loop();
}

void UartConsole::reader_loop() {
    std::array<std::uint8_t, 1024> buffer{};
    while (true) {
        const auto count = uart_read_bytes(kPort, buffer.data(), buffer.size(),
                                           pdMS_TO_TICKS(100));
        if (count > 0) {
            history_->append(buffer.data(), static_cast<std::size_t>(count));
        }
    }
}

}  // namespace stb_buddy
