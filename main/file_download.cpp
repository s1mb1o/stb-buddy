#include "file_download.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

namespace stb_buddy {

namespace {

constexpr auto kTag = "download";
constexpr std::size_t kSerialChunkBytes = 16 * 1024;
constexpr std::size_t kCaptureBytes = 64 * 1024;
constexpr std::size_t kNetworkThresholdBytes = 64 * 1024;
constexpr unsigned kChunkAttempts = 4;
constexpr std::int64_t kShellCheckUs = 5'000'000;
constexpr std::int64_t kNetworkConnectUs = 5'000'000;

class UartTransaction final {
public:
    explicit UartTransaction(UartConsole* uart) : uart_(uart) {
        uart_->begin_transaction();
    }
    ~UartTransaction() { uart_->end_transaction(); }

private:
    UartConsole* uart_;
};

bool line_equals(const std::uint8_t* data, const std::size_t length,
                 const char* expected) {
    const auto expected_length = std::strlen(expected);
    std::size_t start = 0;
    for (std::size_t index = 0; index < length; ++index) {
        if (data[index] != '\r' && data[index] != '\n') {
            continue;
        }
        if (index - start == expected_length &&
            std::memcmp(data + start, expected, expected_length) == 0) {
            return true;
        }
        start = index + 1;
    }
    return false;
}

template <typename Callback>
void each_line(const std::uint8_t* data, const std::size_t length,
               Callback callback) {
    std::size_t start = 0;
    for (std::size_t index = 0; index <= length; ++index) {
        if (index < length && data[index] != '\r' && data[index] != '\n') {
            continue;
        }
        if (index > start) {
            callback(reinterpret_cast<const char*>(data + start),
                     index - start);
        }
        start = index + 1;
    }
}

bool parse_size_and_sha(const std::uint8_t* data, const std::size_t length,
                        std::size_t* size, char* sha) {
    bool have_size = false;
    bool have_sha = false;
    each_line(data, length, [&](const char* line, const std::size_t count) {
        if (!have_size && count > 0 && count < 24) {
            bool digits = true;
            for (std::size_t index = 0; index < count; ++index) {
                digits = digits && std::isdigit(
                                       static_cast<unsigned char>(line[index]));
            }
            if (digits) {
                char text[24]{};
                std::memcpy(text, line, count);
                char* end = nullptr;
                const auto value = std::strtoull(text, &end, 10);
                if (end != text && *end == '\0' && value <= SIZE_MAX) {
                    *size = static_cast<std::size_t>(value);
                    have_size = true;
                }
            }
        }
        if (!have_sha && count >= 65 &&
            std::isspace(static_cast<unsigned char>(line[64]))) {
            bool hexadecimal = true;
            for (std::size_t index = 0; index < 64; ++index) {
                hexadecimal = hexadecimal && std::isxdigit(
                                                   static_cast<unsigned char>(
                                                       line[index]));
            }
            if (hexadecimal) {
                for (std::size_t index = 0; index < 64; ++index) {
                    sha[index] = static_cast<char>(std::tolower(
                        static_cast<unsigned char>(line[index])));
                }
                sha[64] = '\0';
                have_sha = true;
            }
        }
    });
    return have_size && have_sha;
}

bool parse_sha(const std::uint8_t* data, const std::size_t length, char* sha) {
    char parsed[65]{};
    bool found = false;
    each_line(data, length, [&](const char* line, const std::size_t count) {
        if (found || count < 65 ||
            !std::isspace(static_cast<unsigned char>(line[64]))) {
            return;
        }
        bool hexadecimal = true;
        for (std::size_t index = 0; index < 64; ++index) {
            hexadecimal = hexadecimal && std::isxdigit(
                                               static_cast<unsigned char>(
                                                   line[index]));
        }
        if (hexadecimal) {
            for (std::size_t index = 0; index < 64; ++index) {
                parsed[index] = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(line[index])));
            }
            parsed[64] = '\0';
            found = true;
        }
    });
    if (found) {
        std::memcpy(sha, parsed, sizeof(parsed));
    }
    return found;
}

void sha256_hex(const std::uint8_t* data, const std::size_t length,
                char* output) {
    std::uint8_t digest[32]{};
    mbedtls_sha256(data, length, digest, 0);
    for (std::size_t index = 0; index < sizeof(digest); ++index) {
        std::snprintf(output + index * 2, 3, "%02x", digest[index]);
    }
}

bool base64_line(const char* line, const std::size_t length) {
    if (length == 0 || (length % 4) != 0) {
        return false;
    }
    for (std::size_t index = 0; index < length; ++index) {
        const auto value = static_cast<unsigned char>(line[index]);
        if (!(std::isalnum(value) || value == '+' || value == '/' ||
              value == '=')) {
            return false;
        }
    }
    return true;
}

bool decode_chunk(const std::uint8_t* capture, const std::size_t length,
                  std::uint8_t* output, const std::size_t expected,
                  char* board_sha) {
    if (!parse_sha(capture, length, board_sha)) {
        return false;
    }
    bool inside = false;
    bool ended = false;
    bool invalid = false;
    std::size_t written = 0;
    each_line(capture, length, [&](const char* line, const std::size_t count) {
        if (ended || invalid) {
            return;
        }
        if (!inside) {
            constexpr char prefix[] = "begin-base64 ";
            if (count >= sizeof(prefix) - 1 &&
                std::memcmp(line, prefix, sizeof(prefix) - 1) == 0) {
                inside = true;
            }
            return;
        }
        if (count == 4 && std::memcmp(line, "====", 4) == 0) {
            ended = true;
            return;
        }
        if (!base64_line(line, count)) {
            return;
        }
        std::size_t decoded = 0;
        if (written > expected ||
            mbedtls_base64_decode(output + written, expected - written,
                                  &decoded,
                                  reinterpret_cast<const unsigned char*>(line),
                                  count) != 0) {
            invalid = true;
            return;
        }
        written += decoded;
    });
    if (!inside || !ended || invalid || written != expected) {
        return false;
    }
    char local_sha[65]{};
    sha256_hex(output, written, local_sha);
    return std::strcmp(local_sha, board_sha) == 0;
}

bool quote_path(const char* path, char* output, const std::size_t capacity) {
    if (path == nullptr || path[0] == '\0' ||
        std::strlen(path) > kMaximumDownloadPathBytes || capacity < 3) {
        return false;
    }
    std::size_t used = 0;
    output[used++] = '\'';
    for (const unsigned char* cursor =
             reinterpret_cast<const unsigned char*>(path);
         *cursor != '\0'; ++cursor) {
        if (*cursor < 0x20 || *cursor == 0x7f) {
            return false;
        }
        if (*cursor == '\'') {
            constexpr char escape[] = "'\\''";
            if (used + sizeof(escape) - 1 >= capacity) {
                return false;
            }
            std::memcpy(output + used, escape, sizeof(escape) - 1);
            used += sizeof(escape) - 1;
        } else {
            if (used + 1 >= capacity) {
                return false;
            }
            output[used++] = static_cast<char>(*cursor);
        }
    }
    if (used + 2 > capacity) {
        return false;
    }
    output[used++] = '\'';
    output[used] = '\0';
    return true;
}

bool safe_name(const char* path, char* output, const std::size_t capacity) {
    const auto* name = std::strrchr(path, '/');
    name = name == nullptr ? path : name + 1;
    if (name[0] == '\0' || std::strcmp(name, ".") == 0 ||
        std::strcmp(name, "..") == 0 || std::strlen(name) >= capacity) {
        return false;
    }
    std::size_t used = 0;
    for (const unsigned char* cursor =
             reinterpret_cast<const unsigned char*>(name);
         *cursor != '\0'; ++cursor) {
        output[used++] = static_cast<char>(
            std::isalnum(*cursor) || *cursor == '.' || *cursor == '_' ||
                    *cursor == '-'
                ? *cursor
                : '_');
    }
    output[used] = '\0';
    return used > 0;
}

void collect_ips(const std::uint8_t* data, const std::size_t length,
                 DownloadSnapshot* status) {
    status->board_ip_count = 0;
    each_line(data, length, [&](const char* line, const std::size_t count) {
        if (status->board_ip_count == status->board_ips.size()) {
            return;
        }
        for (std::size_t index = 0; index + 5 < count; ++index) {
            const bool modern = index + 5 <= count &&
                                std::memcmp(line + index, "inet ", 5) == 0;
            const bool busybox = index + 10 <= count &&
                                 std::memcmp(line + index, "inet addr:", 10) ==
                                     0;
            if (!modern && !busybox) {
                continue;
            }
            const auto start = index + (busybox ? 10U : 5U);
            char address[16]{};
            std::size_t used = 0;
            for (std::size_t cursor = start;
                 cursor < count && used + 1 < sizeof(address); ++cursor) {
                if (!(std::isdigit(static_cast<unsigned char>(line[cursor])) ||
                      line[cursor] == '.')) {
                    break;
                }
                address[used++] = line[cursor];
            }
            in_addr parsed{};
            if (used == 0 || inet_aton(address, &parsed) == 0 ||
                std::strcmp(address, "127.0.0.1") == 0) {
                continue;
            }
            bool duplicate = false;
            for (std::size_t existing = 0;
                 existing < status->board_ip_count; ++existing) {
                duplicate = duplicate ||
                            std::strcmp(status->board_ips[existing].data(),
                                        address) == 0;
            }
            if (!duplicate) {
                std::snprintf(status->board_ips[status->board_ip_count].data(),
                              status->board_ips[0].size(), "%s", address);
                ++status->board_ip_count;
            }
        }
    });
}

std::int64_t now_us() { return esp_timer_get_time(); }

}  // namespace

const char* download_state_name(const DownloadState state) {
    switch (state) {
        case DownloadState::running:
            return "running";
        case DownloadState::complete:
            return "complete";
        case DownloadState::failed:
            return "failed";
        case DownloadState::idle:
        default:
            return "idle";
    }
}

cJSON* download_snapshot_json(const DownloadSnapshot& snapshot) {
    auto* result = cJSON_CreateObject();
    if (result == nullptr) {
        return nullptr;
    }
    cJSON_AddStringToObject(result, "state",
                            download_state_name(snapshot.state));
    cJSON_AddNumberToObject(result, "generation", snapshot.generation);
    cJSON_AddStringToObject(result, "path", snapshot.path.data());
    cJSON_AddStringToObject(result, "name", snapshot.name.data());
    cJSON_AddNumberToObject(result, "size", snapshot.size);
    cJSON_AddNumberToObject(result, "received", snapshot.received);
    cJSON_AddNumberToObject(result, "seconds",
                            static_cast<double>(snapshot.elapsed_ms) / 1000.0);
    cJSON_AddNumberToObject(result, "retries", snapshot.retries);
    if (snapshot.transport[0] == '\0') {
        cJSON_AddNullToObject(result, "transport");
    } else {
        cJSON_AddStringToObject(result, "transport",
                                snapshot.transport.data());
    }
    if (!snapshot.board_ips_checked) {
        cJSON_AddNullToObject(result, "board_ips");
    } else {
        auto* addresses = cJSON_AddArrayToObject(result, "board_ips");
        for (std::size_t index = 0;
             addresses != nullptr && index < snapshot.board_ip_count; ++index) {
            cJSON_AddItemToArray(
                addresses,
                cJSON_CreateString(snapshot.board_ips[index].data()));
        }
    }
    if (snapshot.sha256_source[0] != '\0') {
        cJSON_AddStringToObject(result, "sha256_source",
                                snapshot.sha256_source.data());
    } else {
        cJSON_AddNullToObject(result, "sha256_source");
    }
    if (snapshot.sha256_local[0] != '\0') {
        cJSON_AddStringToObject(result, "sha256_local",
                                snapshot.sha256_local.data());
    } else {
        cJSON_AddNullToObject(result, "sha256_local");
    }
    if (snapshot.download_url[0] != '\0') {
        char location[96]{};
        std::snprintf(location, sizeof(location), "ram://%s",
                      snapshot.name.data());
        cJSON_AddStringToObject(result, "file", location);
        cJSON_AddStringToObject(result, "download_url",
                                snapshot.download_url.data());
    } else {
        cJSON_AddNullToObject(result, "file");
        cJSON_AddNullToObject(result, "download_url");
    }
    if (snapshot.error[0] != '\0') {
        cJSON_AddStringToObject(result, "error", snapshot.error.data());
    } else {
        cJSON_AddNullToObject(result, "error");
    }
    return result;
}

esp_err_t FileDownloadService::init(RingLog* history, UartConsole* uart,
                                    WifiService* wifi) {
    if (initialized_ || history == nullptr || uart == nullptr || wifi == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    history_ = history;
    uart_ = uart;
    wifi_ = wifi;
    status_.state = DownloadState::idle;
    initialized_ = true;
    return ESP_OK;
}

DownloadStartResult FileDownloadService::start(
    const char* path, const std::uint32_t timeout_seconds,
    std::uint32_t* generation) {
    char quoted[kMaximumDownloadPathBytes * 4 + 3]{};
    char name[81]{};
    if (!initialized_ || generation == nullptr || timeout_seconds == 0 ||
        timeout_seconds > kMaximumDownloadSeconds ||
        !quote_path(path, quoted, sizeof(quoted)) ||
        !safe_name(path, name, sizeof(name))) {
        return DownloadStartResult::invalid;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (status_.state == DownloadState::running) {
        xSemaphoreGive(mutex_);
        return DownloadStartResult::busy;
    }
    heap_caps_free(file_data_);
    file_data_ = nullptr;
    file_size_ = 0;
    const auto next_generation = status_.generation + 1;
    status_ = DownloadSnapshot{};
    status_.state = DownloadState::running;
    status_.generation = next_generation == 0 ? 1 : next_generation;
    std::snprintf(status_.path.data(), status_.path.size(), "%s", path);
    std::snprintf(status_.name.data(), status_.name.size(), "%s", name);
    timeout_seconds_ = timeout_seconds;
    started_us_ = now_us();
    *generation = status_.generation;
    xSemaphoreGive(mutex_);

    // This task holds the quoted path and shell command buffers while the
    // chunk-transfer helpers are active.  Keep their bounded stack use away
    // from the HTTP worker and leave headroom for the lwIP call path.
    if (xTaskCreate(task_entry, "file_download", 12288, this, 7, nullptr) !=
        pdPASS) {
        fail("could not start download task");
        return DownloadStartResult::no_memory;
    }
    return DownloadStartResult::started;
}

DownloadSnapshot FileDownloadService::snapshot() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    auto result = status_;
    if (result.state == DownloadState::running) {
        result.elapsed_ms = static_cast<std::uint32_t>(
            (now_us() - started_us_) / 1000);
    }
    xSemaphoreGive(mutex_);
    return result;
}

bool FileDownloadService::wait(const std::uint32_t generation,
                               const std::uint32_t timeout_seconds,
                               DownloadSnapshot* result) {
    if (result == nullptr) {
        return false;
    }
    const auto deadline = now_us() +
                          static_cast<std::int64_t>(timeout_seconds) * 1'000'000;
    do {
        *result = snapshot();
        if (result->generation != generation ||
            result->state != DownloadState::running) {
            return result->generation == generation;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    } while (now_us() < deadline + 1'000'000);
    *result = snapshot();
    return result->generation == generation &&
           result->state != DownloadState::running;
}

std::size_t FileDownloadService::read_file(
    const char* name, const std::size_t offset, std::uint8_t* output,
    const std::size_t maximum, std::size_t* total, bool* found) {
    if (name == nullptr || output == nullptr || total == nullptr ||
        found == nullptr) {
        return 0;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    *found = status_.state == DownloadState::complete && file_data_ != nullptr &&
             std::strcmp(name, status_.name.data()) == 0;
    *total = *found ? file_size_ : 0;
    const auto count = *found && offset < file_size_
                           ? std::min(maximum, file_size_ - offset)
                           : 0;
    if (count > 0) {
        std::memcpy(output, file_data_ + offset, count);
    }
    xSemaphoreGive(mutex_);
    return count;
}

void FileDownloadService::task_entry(void* context) {
    static_cast<FileDownloadService*>(context)->run();
    vTaskDelete(nullptr);
}

bool FileDownloadService::wait_for_marker(
    std::uint64_t since, const char* marker, const std::int64_t deadline_us,
    std::uint8_t* capture, const std::size_t capacity, std::size_t* length,
    const bool report_failure) {
    *length = 0;
    while (now_us() < deadline_us) {
        const auto span = history_->snapshot();
        if (span.start > since) {
            if (report_failure) {
                fail("UART history was overwritten during download");
            }
            return false;
        }
        if (span.end > since) {
            if (*length == capacity) {
                if (report_failure) {
                    fail("download command produced more than %u bytes",
                         static_cast<unsigned>(capacity));
                }
                return false;
            }
            std::uint64_t actual = 0;
            const auto count = history_->read(
                since, capture + *length,
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    capacity - *length, span.end - since)),
                &actual);
            if (actual != since) {
                if (report_failure) {
                    fail("UART history moved during download");
                }
                return false;
            }
            since += count;
            *length += count;
            if (line_equals(capture, *length, marker)) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (report_failure) {
        fail("download timed out waiting for the STB shell");
    }
    return false;
}

bool FileDownloadService::run_command(
    const char* command, const char* marker, const std::int64_t deadline_us,
    std::uint8_t* capture, const std::size_t capacity, std::size_t* length) {
    UartTransaction transaction(uart_);
    const auto since = history_->snapshot().end;
    std::size_t written = 0;
    const auto command_length = std::strlen(command);
    if (uart_->write(reinterpret_cast<const std::uint8_t*>(command),
                     command_length, &written) != ESP_OK ||
        written != command_length) {
        fail("could not write download command to UART");
        return false;
    }
    const std::uint8_t enter = '\r';
    if (uart_->write(&enter, 1, &written) != ESP_OK || written != 1) {
        fail("could not send Enter to the STB shell");
        return false;
    }
    return wait_for_marker(since, marker, deadline_us, capture, capacity,
                           length, true);
}

bool FileDownloadService::interrupt_and_recover(
    const std::uint64_t since, const char* marker, std::uint8_t* capture,
    const std::size_t capture_capacity) {
    const std::uint8_t interrupt = 0x03;
    std::size_t written = 0;
    if (uart_->write(&interrupt, 1, &written) != ESP_OK || written != 1) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    char command[64]{};
    const auto length = std::snprintf(command, sizeof(command), "echo %s\r",
                                      marker);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(command) ||
        uart_->write(reinterpret_cast<const std::uint8_t*>(command),
                     static_cast<std::size_t>(length), &written) != ESP_OK ||
        written != static_cast<std::size_t>(length)) {
        return false;
    }
    std::size_t ignored = 0;
    return wait_for_marker(since, marker, now_us() + kNetworkConnectUs,
                           capture, capture_capacity, &ignored, false);
}

void FileDownloadService::set_progress(const std::size_t received) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    status_.received = received;
    xSemaphoreGive(mutex_);
}

void FileDownloadService::fail(const char* format, ...) {
    char message[256]{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (status_.state == DownloadState::running) {
        status_.state = DownloadState::failed;
        status_.elapsed_ms = static_cast<std::uint32_t>(
            (now_us() - started_us_) / 1000);
        std::snprintf(status_.error.data(), status_.error.size(), "%s", message);
    }
    xSemaphoreGive(mutex_);
    ESP_LOGW(kTag, "%s", message);
}

bool FileDownloadService::try_network_transfer(
    const char* quoted_path, const std::size_t size,
    std::uint8_t* destination, const std::int64_t deadline_us,
    const char* marker, std::uint8_t* capture,
    const std::size_t capture_capacity, bool* connected) {
    *connected = false;
    const auto wifi = wifi_->snapshot();
    if (wifi.ip[0] == '\0') {
        return true;
    }
    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listener < 0) {
        fail("could not create nc listener");
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
            0 ||
        listen(listener, 1) != 0) {
        close(listener);
        fail("could not start nc listener");
        return false;
    }
    socklen_t address_length = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                    &address_length) != 0) {
        close(listener);
        fail("could not inspect nc listener");
        return false;
    }

    char command[1400]{};
    std::snprintf(command, sizeof(command), "cat %s | nc %s %u; echo %s",
                  quoted_path, wifi.ip.data(), ntohs(address.sin_port), marker);
    UartTransaction transaction(uart_);
    const auto since = history_->snapshot().end;
    std::size_t written = 0;
    const auto command_length = std::strlen(command);
    if (uart_->write(reinterpret_cast<const std::uint8_t*>(command),
                     command_length, &written) != ESP_OK ||
        written != command_length) {
        close(listener);
        fail("could not start nc on the STB");
        return false;
    }
    const std::uint8_t enter = '\r';
    uart_->write(&enter, 1, &written);

    fd_set reads;
    FD_ZERO(&reads);
    FD_SET(listener, &reads);
    const auto remaining = std::max<std::int64_t>(
        0, std::min(kNetworkConnectUs, deadline_us - now_us()));
    timeval timeout{.tv_sec = static_cast<time_t>(remaining / 1'000'000),
                    .tv_usec = static_cast<suseconds_t>(remaining % 1'000'000)};
    const auto selected = select(listener + 1, &reads, nullptr, nullptr, &timeout);
    if (selected <= 0) {
        const auto recovered = interrupt_and_recover(
            since, marker, capture, capture_capacity);
        close(listener);
        if (!recovered) {
            fail("could not recover the STB shell after nc timeout");
            return false;
        }
        return true;
    }

    const int connection = accept(listener, nullptr, nullptr);
    close(listener);
    if (connection < 0) {
        fail("could not accept STB nc connection");
        return false;
    }
    *connected = true;
    std::size_t received = 0;
    while (received < size && now_us() < deadline_us) {
        FD_ZERO(&reads);
        FD_SET(connection, &reads);
        const auto left = std::max<std::int64_t>(0, deadline_us - now_us());
        timeval receive_timeout{
            .tv_sec = static_cast<time_t>(left / 1'000'000),
            .tv_usec = static_cast<suseconds_t>(left % 1'000'000)};
        if (select(connection + 1, &reads, nullptr, nullptr,
                   &receive_timeout) <= 0) {
            break;
        }
        const auto count = recv(connection, destination + received,
                                size - received, 0);
        if (count <= 0) {
            break;
        }
        received += static_cast<std::size_t>(count);
        set_progress(received);
    }
    shutdown(connection, SHUT_RDWR);
    close(connection);
    std::size_t capture_length = 0;
    if (!wait_for_marker(since, marker, deadline_us, capture,
                         capture_capacity, &capture_length, false)) {
        interrupt_and_recover(since, marker, capture, capture_capacity);
        fail("download timed out waiting for nc to return to the STB shell");
        return false;
    }
    if (received != size) {
        fail("nc transfer ended after %u of %u bytes",
             static_cast<unsigned>(received), static_cast<unsigned>(size));
        return false;
    }
    return true;
}

bool FileDownloadService::serial_transfer(
    const char* quoted_path, const std::size_t size,
    std::uint8_t* destination, const std::int64_t deadline_us,
    const char* marker, std::uint8_t* capture,
    const std::size_t capture_capacity, std::uint8_t* chunk) {
    const auto chunks = (size + kSerialChunkBytes - 1) / kSerialChunkBytes;
    for (std::size_t number = 0; number < chunks; ++number) {
        const auto expected =
            std::min(kSerialChunkBytes, size - number * kSerialChunkBytes);
        char command[2304]{};
        const auto count = std::snprintf(
            command, sizeof(command),
            "dd if=%s bs=%u skip=%u count=1 2>&- | sha256sum; "
            "dd if=%s bs=%u skip=%u count=1 2>&- | uuencode -m x; echo %s",
            quoted_path, static_cast<unsigned>(kSerialChunkBytes),
            static_cast<unsigned>(number), quoted_path,
            static_cast<unsigned>(kSerialChunkBytes),
            static_cast<unsigned>(number), marker);
        if (count < 0 || static_cast<std::size_t>(count) >= sizeof(command)) {
            fail("download path is too long for a chunk command");
            return false;
        }
        bool valid = false;
        for (unsigned attempt = 0; attempt < kChunkAttempts; ++attempt) {
            std::size_t capture_length = 0;
            if (!run_command(command, marker, deadline_us, capture,
                             capture_capacity, &capture_length)) {
                return false;
            }
            char board_sha[65]{};
            if (decode_chunk(capture, capture_length, chunk, expected,
                             board_sha)) {
                std::memcpy(destination + number * kSerialChunkBytes, chunk,
                            expected);
                set_progress(number * kSerialChunkBytes + expected);
                valid = true;
                break;
            }
            xSemaphoreTake(mutex_, portMAX_DELAY);
            ++status_.retries;
            xSemaphoreGive(mutex_);
            ESP_LOGW(kTag, "chunk %u failed verification (attempt %u)",
                     static_cast<unsigned>(number), attempt + 1);
        }
        if (!valid) {
            fail("chunk %u was damaged four times",
                 static_cast<unsigned>(number));
            return false;
        }
    }
    return true;
}

void FileDownloadService::run() {
    DownloadSnapshot initial = snapshot();
    const auto started_us = started_us_;
    const auto deadline_us =
        started_us + static_cast<std::int64_t>(timeout_seconds_) * 1'000'000;
    auto* capture = static_cast<std::uint8_t*>(heap_caps_malloc(
        kCaptureBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* chunk = static_cast<std::uint8_t*>(heap_caps_malloc(
        kSerialChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (capture == nullptr || chunk == nullptr) {
        heap_caps_free(capture);
        heap_caps_free(chunk);
        fail("not enough PSRAM for download buffers");
        return;
    }

    char quoted[kMaximumDownloadPathBytes * 4 + 3]{};
    if (!quote_path(initial.path.data(), quoted, sizeof(quoted))) {
        fail("invalid STB path");
        heap_caps_free(capture);
        heap_caps_free(chunk);
        return;
    }
    char marker[48]{};
    std::snprintf(marker, sizeof(marker), "__STB_BUDDY_DL_%08lx__",
                  static_cast<unsigned long>(initial.generation));
    char command[2304]{};
    std::snprintf(command, sizeof(command),
                  "echo READY_$((40+2)); echo %s", marker);
    std::size_t capture_length = 0;
    if (!run_command(command, marker,
                     std::min(deadline_us, now_us() + kShellCheckUs), capture,
                     kCaptureBytes, &capture_length) ||
        !line_equals(capture, capture_length, "READY_42")) {
        if (snapshot().state == DownloadState::running) {
            fail("the console is not at a shell prompt (expected READY_42)");
        }
        heap_caps_free(capture);
        heap_caps_free(chunk);
        return;
    }

    std::snprintf(command, sizeof(command),
                  "wc -c < %s; sha256sum %s; echo %s", quoted, quoted,
                  marker);
    if (!run_command(command, marker, deadline_us, capture, kCaptureBytes,
                     &capture_length)) {
        heap_caps_free(capture);
        heap_caps_free(chunk);
        return;
    }
    std::size_t size = 0;
    char source_sha[65]{};
    if (!parse_size_and_sha(capture, capture_length, &size, source_sha)) {
        fail("cannot read file size and SHA-256 on the STB");
        heap_caps_free(capture);
        heap_caps_free(chunk);
        return;
    }
    if (size > kMaximumDownloadedFileBytes) {
        fail("file is %u bytes; stb-buddy limit is %u bytes",
             static_cast<unsigned>(size),
             static_cast<unsigned>(kMaximumDownloadedFileBytes));
        heap_caps_free(capture);
        heap_caps_free(chunk);
        return;
    }
    auto* destination = static_cast<std::uint8_t*>(heap_caps_malloc(
        std::max<std::size_t>(size, 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (destination == nullptr) {
        fail("not enough PSRAM for a %u-byte file",
             static_cast<unsigned>(size));
        heap_caps_free(capture);
        heap_caps_free(chunk);
        return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    status_.size = size;
    std::snprintf(status_.sha256_source.data(), status_.sha256_source.size(),
                  "%s", source_sha);
    xSemaphoreGive(mutex_);

    bool ok = true;
    bool used_network = false;
    if (size > kNetworkThresholdBytes) {
        std::snprintf(command, sizeof(command), "ifconfig; echo %s", marker);
        ok = run_command(command, marker, deadline_us, capture, kCaptureBytes,
                         &capture_length);
        if (ok) {
            xSemaphoreTake(mutex_, portMAX_DELAY);
            status_.board_ips_checked = true;
            collect_ips(capture, capture_length, &status_);
            xSemaphoreGive(mutex_);
            ok = try_network_transfer(quoted, size, destination, deadline_us,
                                      marker, capture, kCaptureBytes,
                                      &used_network);
        }
    }
    if (ok && !used_network) {
        ok = serial_transfer(quoted, size, destination, deadline_us, marker,
                             capture, kCaptureBytes, chunk);
    }

    char local_sha[65]{};
    if (ok) {
        sha256_hex(destination, size, local_sha);
        if (std::strcmp(local_sha, source_sha) != 0) {
            fail("SHA-256 mismatch: STB %s, copy %s", source_sha, local_sha);
            ok = false;
        }
    }
    if (ok) {
        const auto wifi = wifi_->snapshot();
        xSemaphoreTake(mutex_, portMAX_DELAY);
        file_data_ = destination;
        file_size_ = size;
        destination = nullptr;
        status_.state = DownloadState::complete;
        status_.received = size;
        status_.elapsed_ms = static_cast<std::uint32_t>(
            (now_us() - started_us) / 1000);
        status_.error[0] = '\0';
        std::snprintf(status_.transport.data(), status_.transport.size(), "%s",
                      used_network ? "nc" : "serial");
        std::snprintf(status_.sha256_local.data(), status_.sha256_local.size(),
                      "%s", local_sha);
        const char* ip = wifi.ip[0] == '\0' ? "192.168.4.1" : wifi.ip.data();
        std::snprintf(status_.download_url.data(),
                      status_.download_url.size(),
                      "http://%s/api/downloads/%s", ip, status_.name.data());
        xSemaphoreGive(mutex_);
        ESP_LOGI(kTag, "downloaded %s: %u bytes over %s in %.1f s",
                 initial.path.data(), static_cast<unsigned>(size),
                 used_network ? "nc" : "serial",
                 static_cast<double>(status_.elapsed_ms) / 1000.0);
    }
    heap_caps_free(destination);
    heap_caps_free(capture);
    heap_caps_free(chunk);
}

}  // namespace stb_buddy
