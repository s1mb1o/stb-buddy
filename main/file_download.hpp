#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "cJSON.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ring_log.hpp"
#include "uart_console.hpp"
#include "wifi_service.hpp"

namespace stb_buddy {

constexpr std::size_t kMaximumDownloadedFileBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaximumDownloadPathBytes = 240;
constexpr std::uint32_t kMaximumDownloadSeconds = 600;

enum class DownloadState : std::uint8_t {
    idle,
    running,
    complete,
    failed,
};

const char* download_state_name(DownloadState state);

struct DownloadSnapshot {
    DownloadState state;
    std::uint32_t generation;
    std::array<char, kMaximumDownloadPathBytes + 1> path;
    std::array<char, 81> name;
    std::size_t size;
    std::size_t received;
    std::uint32_t elapsed_ms;
    std::uint32_t retries;
    std::array<char, 8> transport;
    std::array<char, 65> sha256_source;
    std::array<char, 65> sha256_local;
    std::array<std::array<char, 16>, 4> board_ips;
    std::size_t board_ip_count;
    bool board_ips_checked;
    std::array<char, 128> download_url;
    std::array<char, 256> error;
};

cJSON* download_snapshot_json(const DownloadSnapshot& snapshot);

enum class DownloadStartResult : std::uint8_t {
    started,
    busy,
    invalid,
    no_memory,
};

class FileDownloadService final {
public:
    FileDownloadService() = default;
    FileDownloadService(const FileDownloadService&) = delete;
    FileDownloadService& operator=(const FileDownloadService&) = delete;

    esp_err_t init(RingLog* history, UartConsole* uart, WifiService* wifi);
    DownloadStartResult start(const char* path, std::uint32_t timeout_seconds,
                              std::uint32_t* generation);
    DownloadSnapshot snapshot();
    bool wait(std::uint32_t generation, std::uint32_t timeout_seconds,
              DownloadSnapshot* result);
    std::size_t read_file(const char* name, std::size_t offset,
                          std::uint8_t* output, std::size_t maximum,
                          std::size_t* total, bool* found);

private:
    static void task_entry(void* context);
    void run();
    bool run_command(const char* command, const char* marker,
                     std::int64_t deadline_us, std::uint8_t* capture,
                     std::size_t capacity, std::size_t* length);
    bool wait_for_marker(std::uint64_t since, const char* marker,
                         std::int64_t deadline_us, std::uint8_t* capture,
                         std::size_t capacity, std::size_t* length,
                         bool report_failure);
    bool interrupt_and_recover(std::uint64_t since, const char* marker,
                               std::uint8_t* capture,
                               std::size_t capture_capacity);
    bool try_network_transfer(const char* quoted_path, std::size_t size,
                              std::uint8_t* destination,
                              std::int64_t deadline_us, const char* marker,
                              std::uint8_t* capture,
                              std::size_t capture_capacity,
                              bool* connected);
    bool serial_transfer(const char* quoted_path, std::size_t size,
                         std::uint8_t* destination, std::int64_t deadline_us,
                         const char* marker, std::uint8_t* capture,
                         std::size_t capture_capacity,
                         std::uint8_t* chunk);
    void fail(const char* format, ...);
    void set_progress(std::size_t received);

    RingLog* history_{nullptr};
    UartConsole* uart_{nullptr};
    WifiService* wifi_{nullptr};
    SemaphoreHandle_t mutex_{nullptr};
    DownloadSnapshot status_{};
    std::uint8_t* file_data_{nullptr};
    std::size_t file_size_{0};
    std::uint32_t timeout_seconds_{0};
    std::int64_t started_us_{0};
    bool initialized_{false};
};

}  // namespace stb_buddy
