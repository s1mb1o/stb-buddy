#include "http_service.hpp"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace stb_buddy {

namespace {

constexpr auto kTag = "http";
constexpr std::size_t kOtaBufferBytes = 4096;
constexpr std::size_t kMaximumLogRead = 256 * 1024;
constexpr std::size_t kLogChunkBytes = 16 * 1024;
constexpr std::size_t kMaximumUartWrite = 4096;
constexpr std::uint16_t kHttpPort = 80;
constexpr UBaseType_t kMcpWorkerCount = 2;
constexpr std::uint32_t kMcpWorkerStackBytes = 12 * 1024;

extern const std::uint8_t web_index_html_start[]
    asm("_binary_index_html_start");
extern const std::uint8_t web_index_html_end[]
    asm("_binary_index_html_end");
extern const std::uint8_t web_app_css_start[] asm("_binary_app_css_start");
extern const std::uint8_t web_app_css_end[] asm("_binary_app_css_end");
extern const std::uint8_t web_app_js_start[] asm("_binary_app_js_start");
extern const std::uint8_t web_app_js_end[] asm("_binary_app_js_end");
extern const std::uint8_t web_help_html_start[]
    asm("_binary_help_html_start");
extern const std::uint8_t web_help_html_end[] asm("_binary_help_html_end");
extern const std::uint8_t web_help_js_start[] asm("_binary_help_js_start");
extern const std::uint8_t web_help_js_end[] asm("_binary_help_js_end");
extern const std::uint8_t web_openapi_json_start[]
    asm("_binary_openapi_json_start");
extern const std::uint8_t web_openapi_json_end[]
    asm("_binary_openapi_json_end");
extern const std::uint8_t web_setup_html_start[]
    asm("_binary_setup_html_start");
extern const std::uint8_t web_setup_html_end[]
    asm("_binary_setup_html_end");
extern const std::uint8_t web_setup_js_start[]
    asm("_binary_setup_js_start");
extern const std::uint8_t web_setup_js_end[]
    asm("_binary_setup_js_end");
extern const std::uint8_t web_vendor_xterm_css_start[]
    asm("_binary_xterm_css_start");
extern const std::uint8_t web_vendor_xterm_css_end[]
    asm("_binary_xterm_css_end");
extern const std::uint8_t web_vendor_xterm_js_start[]
    asm("_binary_xterm_js_start");
extern const std::uint8_t web_vendor_xterm_js_end[]
    asm("_binary_xterm_js_end");
extern const std::uint8_t web_vendor_addon_fit_js_start[]
    asm("_binary_addon_fit_js_start");
extern const std::uint8_t web_vendor_addon_fit_js_end[]
    asm("_binary_addon_fit_js_end");

esp_err_t send_json(httpd_req_t* request, const char* body) {
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, body);
}

esp_err_t send_json_error(httpd_req_t* request, const char* status,
                          const char* error) {
    char body[192]{};
    std::snprintf(body, sizeof(body), "{\"error\":\"%s\"}", error);
    httpd_resp_set_status(request, status);
    return send_json(request, body);
}

esp_err_t receive_body(httpd_req_t* request, char* output,
                       const std::size_t length) {
    std::size_t received = 0;
    unsigned timeouts = 0;
    while (received < length) {
        const auto result = httpd_req_recv(request, output + received,
                                           length - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts == 3) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (result <= 0) {
            return ESP_FAIL;
        }
        timeouts = 0;
        received += static_cast<std::size_t>(result);
    }
    return ESP_OK;
}

esp_err_t send_embedded(httpd_req_t* request, const std::uint8_t* start,
                        const std::uint8_t* end, const char* content_type,
                        const bool cacheable) {
    auto length = static_cast<std::size_t>(end - start);
    if (length > 0 && start[length - 1] == 0) {
        --length;
    }
    httpd_resp_set_type(request, content_type);
    httpd_resp_set_hdr(request, "Cache-Control",
                       cacheable ? "no-cache" : "no-store");
    return httpd_resp_send(request, reinterpret_cast<const char*>(start),
                           static_cast<std::ptrdiff_t>(length));
}

bool query_unsigned(httpd_req_t* request, const char* key,
                    std::uint64_t* value, bool* present) {
    *present = false;
    const auto query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0) {
        return true;
    }
    if (query_length >= 160) {
        return false;
    }
    char query[160]{};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char text[32]{};
    const auto result = httpd_query_key_value(query, key, text, sizeof(text));
    if (result == ESP_ERR_NOT_FOUND) {
        return true;
    }
    if (result != ESP_OK || text[0] == '\0' || text[0] == '-') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = parsed;
    *present = true;
    return true;
}

esp_err_t send_escaped_json_string(httpd_req_t* request, const char* value) {
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "\"", 1), kTag,
                        "send JSON quote");
    char chunk[128]{};
    std::size_t used = 0;
    for (const unsigned char* cursor =
             reinterpret_cast<const unsigned char*>(value);
         *cursor != '\0'; ++cursor) {
        char escaped[7]{};
        const char* source = escaped;
        std::size_t count = 0;
        if (*cursor == '"' || *cursor == '\\') {
            escaped[0] = '\\';
            escaped[1] = static_cast<char>(*cursor);
            count = 2;
        } else if (*cursor < 0x20) {
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
            count = 6;
        } else {
            escaped[0] = static_cast<char>(*cursor);
            count = 1;
        }
        if (used + count > sizeof(chunk)) {
            ESP_RETURN_ON_ERROR(
                httpd_resp_send_chunk(request, chunk, used), kTag,
                "send JSON string chunk");
            used = 0;
        }
        std::memcpy(chunk + used, source, count);
        used += count;
    }
    if (used > 0) {
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, chunk, used), kTag,
                            "send final JSON string chunk");
    }
    return httpd_resp_send_chunk(request, "\"", 1);
}

const char* ota_state_name(const esp_ota_img_states_t state) {
    switch (state) {
        case ESP_OTA_IMG_NEW:
            return "new";
        case ESP_OTA_IMG_PENDING_VERIFY:
            return "pending_verify";
        case ESP_OTA_IMG_VALID:
            return "valid";
        case ESP_OTA_IMG_INVALID:
            return "invalid";
        case ESP_OTA_IMG_ABORTED:
            return "aborted";
        case ESP_OTA_IMG_UNDEFINED:
            return "undefined";
        default:
            return "unknown";
    }
}

bool add_partition_json(cJSON* parent, const char* name,
                        const esp_partition_t* partition) {
    if (partition == nullptr) {
        return cJSON_AddNullToObject(parent, name) != nullptr;
    }
    auto* json = cJSON_AddObjectToObject(parent, name);
    if (json == nullptr) {
        return false;
    }

    bool complete = cJSON_AddStringToObject(json, "label", partition->label) !=
                    nullptr;
    complete = cJSON_AddNumberToObject(json, "address", partition->address) !=
                   nullptr &&
               complete;
    complete = cJSON_AddNumberToObject(json, "size", partition->size) !=
                   nullptr &&
               complete;

    esp_app_desc_t description{};
    if (esp_ota_get_partition_description(partition, &description) == ESP_OK) {
        complete = cJSON_AddStringToObject(json, "project",
                                           description.project_name) != nullptr &&
                   complete;
        complete = cJSON_AddStringToObject(json, "version",
                                           description.version) != nullptr &&
                   complete;
    } else {
        complete = cJSON_AddNullToObject(json, "project") != nullptr && complete;
        complete = cJSON_AddNullToObject(json, "version") != nullptr && complete;
    }
    return complete;
}

esp_err_t send_cjson(httpd_req_t* request, cJSON* json) {
    if (json == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    char* body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    const auto result = send_json(request, body);
    cJSON_free(body);
    return result;
}

void delayed_restart(void*) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

}  // namespace

esp_err_t HttpService::start(RingLog* history, UartConsole* uart,
                             IrBlaster* ir, WifiService* wifi,
                             RemoteStore* remotes, McpService* mcp,
                             FileDownloadService* downloads) {
    if (history == nullptr || uart == nullptr || ir == nullptr ||
        wifi == nullptr || remotes == nullptr || mcp == nullptr ||
        downloads == nullptr ||
        server_ != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    history_ = history;
    uart_ = uart;
    ir_ = ir;
    wifi_ = wifi;
    remotes_ = remotes;
    mcp_ = mcp;
    downloads_ = downloads;
    mcp_worker_slots_ =
        xSemaphoreCreateCounting(kMcpWorkerCount, kMcpWorkerCount);
    if (mcp_worker_slots_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kHttpPort;
    config.stack_size = 8192;
    config.max_open_sockets = 8;
    config.max_uri_handlers = 29;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&server_, &config), kTag,
                        "start HTTP server");

    httpd_uri_t index{};
    index.uri = "/";
    index.method = HTTP_GET;
    index.handler = index_handler;
    index.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &index), kTag,
                        "register WebUI endpoint");

    httpd_uri_t docs{};
    docs.uri = "/docs";
    docs.method = HTTP_GET;
    docs.handler = docs_handler;
    docs.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &docs), kTag,
                        "register API documentation endpoint");

    httpd_uri_t openapi{};
    openapi.uri = "/openapi.json";
    openapi.method = HTTP_GET;
    openapi.handler = openapi_handler;
    openapi.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &openapi), kTag,
                        "register OpenAPI endpoint");

    httpd_uri_t static_assets{};
    static_assets.uri = "/static/*";
    static_assets.method = HTTP_GET;
    static_assets.handler = static_asset_handler;
    static_assets.user_ctx = this;
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server_, &static_assets), kTag,
        "register WebUI assets");

    httpd_uri_t health{};
    health.uri = "/health";
    health.method = HTTP_GET;
    health.handler = health_handler;
    health.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &health), kTag,
                        "register health endpoint");

    httpd_uri_t log_read{};
    log_read.uri = "/api/read";
    log_read.method = HTTP_GET;
    log_read.handler = log_read_handler;
    log_read.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &log_read), kTag,
                        "register log read endpoint");

    httpd_uri_t uart_write{};
    uart_write.uri = "/api/uart/write";
    uart_write.method = HTTP_POST;
    uart_write.handler = uart_write_handler;
    uart_write.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &uart_write), kTag,
                        "register UART write endpoint");

    httpd_uri_t uart_events{};
    uart_events.uri = "/api/uart/events";
    uart_events.method = HTTP_GET;
    uart_events.handler = uart_events_handler;
    uart_events.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &uart_events),
                        kTag, "register UART event endpoint");

    httpd_uri_t uart_configure{};
    uart_configure.uri = "/api/uart/configure";
    uart_configure.method = HTTP_POST;
    uart_configure.handler = uart_configure_handler;
    uart_configure.user_ctx = this;
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server_, &uart_configure), kTag,
        "register UART configure endpoint");

    httpd_uri_t clear_history{};
    clear_history.uri = "/api/clear_history";
    clear_history.method = HTTP_POST;
    clear_history.handler = clear_history_handler;
    clear_history.user_ctx = this;
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server_, &clear_history), kTag,
        "register clear history endpoint");

    httpd_uri_t log_download{};
    log_download.uri = "/api/log";
    log_download.method = HTTP_GET;
    log_download.handler = log_download_handler;
    log_download.user_ctx = this;
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server_, &log_download), kTag,
        "register log download endpoint");

    httpd_uri_t file_download_start{};
    file_download_start.uri = "/api/download";
    file_download_start.method = HTTP_POST;
    file_download_start.handler = file_download_start_handler;
    file_download_start.user_ctx = this;
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server_, &file_download_start), kTag,
        "register file download start endpoint");

    httpd_uri_t file_download_status{};
    file_download_status.uri = "/api/download";
    file_download_status.method = HTTP_GET;
    file_download_status.handler = file_download_status_handler;
    file_download_status.user_ctx = this;
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server_, &file_download_status), kTag,
        "register file download status endpoint");

    httpd_uri_t downloaded_file{};
    downloaded_file.uri = "/api/downloads/*";
    downloaded_file.method = HTTP_GET;
    downloaded_file.handler = downloaded_file_handler;
    downloaded_file.user_ctx = this;
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server_, &downloaded_file), kTag,
        "register downloaded file endpoint");

    httpd_uri_t wifi_status{};
    wifi_status.uri = "/api/wifi";
    wifi_status.method = HTTP_GET;
    wifi_status.handler = wifi_status_handler;
    wifi_status.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &wifi_status), kTag,
                        "register Wi-Fi status endpoint");

    httpd_uri_t wifi_save{};
    wifi_save.uri = "/api/wifi";
    wifi_save.method = HTTP_POST;
    wifi_save.handler = wifi_save_handler;
    wifi_save.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &wifi_save), kTag,
                        "register Wi-Fi save endpoint");

    httpd_uri_t remotes_upload{};
    remotes_upload.uri = "/api/remotes";
    remotes_upload.method = HTTP_PUT;
    remotes_upload.handler = remotes_upload_handler;
    remotes_upload.user_ctx = this;
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server_, &remotes_upload), kTag,
        "register remote upload endpoint");

    httpd_uri_t remotes_download{};
    remotes_download.uri = "/api/remotes/config";
    remotes_download.method = HTTP_GET;
    remotes_download.handler = remotes_download_handler;
    remotes_download.user_ctx = this;
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server_, &remotes_download), kTag,
        "register remote download endpoint");

    httpd_uri_t remotes_list{};
    remotes_list.uri = "/api/remotes";
    remotes_list.method = HTTP_GET;
    remotes_list.handler = remotes_list_handler;
    remotes_list.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &remotes_list),
                        kTag, "register remote list endpoint");

    httpd_uri_t ir_send{};
    ir_send.uri = "/api/ir/send";
    ir_send.method = HTTP_POST;
    ir_send.handler = ir_send_handler;
    ir_send.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &ir_send), kTag,
                        "register IR send endpoint");

    httpd_uri_t ir_events{};
    ir_events.uri = "/api/ir/events";
    ir_events.method = HTTP_GET;
    ir_events.handler = ir_events_handler;
    ir_events.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &ir_events), kTag,
                        "register IR event endpoint");

    httpd_uri_t ota_status{};
    ota_status.uri = "/api/ota";
    ota_status.method = HTTP_GET;
    ota_status.handler = ota_status_handler;
    ota_status.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &ota_status), kTag,
                        "register OTA status endpoint");

    httpd_uri_t ota_upload{};
    ota_upload.uri = "/api/ota";
    ota_upload.method = HTTP_POST;
    ota_upload.handler = ota_upload_handler;
    ota_upload.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &ota_upload), kTag,
                        "register OTA upload endpoint");

    httpd_uri_t ota_reboot{};
    ota_reboot.uri = "/api/ota/reboot";
    ota_reboot.method = HTTP_POST;
    ota_reboot.handler = ota_reboot_handler;
    ota_reboot.user_ctx = this;
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server_, &ota_reboot), kTag,
                        "register OTA reboot endpoint");

    httpd_uri_t mcp_endpoint{};
    mcp_endpoint.uri = "/mcp";
    mcp_endpoint.method = HTTP_POST;
    mcp_endpoint.handler = mcp_handler;
    mcp_endpoint.user_ctx = this;
    ESP_RETURN_ON_ERROR(
        httpd_register_uri_handler(server_, &mcp_endpoint), kTag,
        "register MCP endpoint");

    ESP_LOGI(kTag, "setup, WebUI, API, OTA and MCP ready on HTTP port %u",
             static_cast<unsigned>(kHttpPort));
    return ESP_OK;
}

esp_err_t HttpService::index_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    if (self->wifi_->setup_mode()) {
        return send_embedded(request, web_setup_html_start, web_setup_html_end,
                             "text/html; charset=utf-8", false);
    }
    return send_embedded(request, web_index_html_start, web_index_html_end,
                         "text/html; charset=utf-8", false);
}

esp_err_t HttpService::docs_handler(httpd_req_t* request) {
    return send_embedded(request, web_help_html_start, web_help_html_end,
                         "text/html; charset=utf-8", false);
}

esp_err_t HttpService::openapi_handler(httpd_req_t* request) {
    return send_embedded(request, web_openapi_json_start,
                         web_openapi_json_end,
                         "application/json; charset=utf-8", false);
}

esp_err_t HttpService::static_asset_handler(httpd_req_t* request) {
    if (std::strcmp(request->uri, "/static/app.css") == 0) {
        return send_embedded(request, web_app_css_start, web_app_css_end,
                             "text/css; charset=utf-8", true);
    }
    if (std::strcmp(request->uri, "/static/app.js") == 0) {
        return send_embedded(request, web_app_js_start, web_app_js_end,
                             "text/javascript; charset=utf-8", true);
    }
    if (std::strcmp(request->uri, "/static/help.js") == 0) {
        return send_embedded(request, web_help_js_start, web_help_js_end,
                             "text/javascript; charset=utf-8", true);
    }
    if (std::strcmp(request->uri, "/static/setup.js") == 0) {
        return send_embedded(request, web_setup_js_start, web_setup_js_end,
                             "text/javascript; charset=utf-8", true);
    }
    if (std::strcmp(request->uri, "/static/xterm.css") == 0) {
        return send_embedded(request, web_vendor_xterm_css_start,
                             web_vendor_xterm_css_end,
                             "text/css; charset=utf-8", true);
    }
    if (std::strcmp(request->uri, "/static/xterm.js") == 0) {
        return send_embedded(request, web_vendor_xterm_js_start,
                             web_vendor_xterm_js_end,
                             "text/javascript; charset=utf-8", true);
    }
    if (std::strcmp(request->uri, "/static/addon-fit.js") == 0) {
        return send_embedded(request, web_vendor_addon_fit_js_start,
                             web_vendor_addon_fit_js_end,
                             "text/javascript; charset=utf-8", true);
    }
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "asset not found");
}

esp_err_t HttpService::health_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    const auto history = self->history_->snapshot();
    const auto wifi = self->wifi_->snapshot();
    auto* response = cJSON_CreateObject();
    if (response == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    auto* wifi_json = cJSON_AddObjectToObject(response, "wifi");
    auto* uart_json = cJSON_AddObjectToObject(response, "uart");
    auto* history_json = cJSON_AddObjectToObject(response, "history");
    auto* ir_json = cJSON_AddObjectToObject(response, "ir");
    if (wifi_json == nullptr || uart_json == nullptr ||
        history_json == nullptr || ir_json == nullptr) {
        cJSON_Delete(response);
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    cJSON_AddStringToObject(response, "name", "stb-buddy");
    cJSON_AddStringToObject(response, "version",
                            esp_app_get_description()->version);
    cJSON_AddStringToObject(wifi_json, "mode",
                            wifi.setup_mode ? "setup-ap" : "station");
    cJSON_AddBoolToObject(wifi_json, "connected", wifi.connected);
    cJSON_AddStringToObject(wifi_json, "ssid", wifi.ssid.data());
    cJSON_AddStringToObject(wifi_json, "ip", wifi.ip.data());
    cJSON_AddBoolToObject(uart_json, "connected", self->uart_->connected());
    cJSON_AddNumberToObject(uart_json, "port", 1);
    cJSON_AddNumberToObject(uart_json, "rx_gpio", 1);
    cJSON_AddNumberToObject(uart_json, "tx_gpio", 2);
    const auto uart_config = self->uart_->configuration();
    cJSON_AddNumberToObject(uart_json, "baud", uart_config.baud);
    cJSON_AddNumberToObject(uart_json, "data_bits", uart_config.data_bits);
    cJSON_AddStringToObject(uart_json, "parity",
                            uart_parity_name(uart_config.parity));
    cJSON_AddNumberToObject(
        uart_json, "stop_bits",
        static_cast<double>(uart_config.stop_bits_x2) / 2.0);
    cJSON_AddNumberToObject(history_json, "start",
                            static_cast<double>(history.start));
    cJSON_AddNumberToObject(history_json, "end",
                            static_cast<double>(history.end));
    cJSON_AddNumberToObject(history_json, "bytes", history.size);
    cJSON_AddNumberToObject(history_json, "capacity", history.capacity);
    cJSON_AddBoolToObject(ir_json, "ready", self->ir_->ready());
    cJSON_AddNumberToObject(ir_json, "gpio", 47);
    cJSON_AddNumberToObject(ir_json, "remotes",
                            self->remotes_->remote_count());
    cJSON_AddNumberToObject(ir_json, "keys", self->remotes_->key_count());
    return send_cjson(request, response);
}

esp_err_t HttpService::log_read_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    std::uint64_t since = 0;
    std::uint64_t maximum_value = 32768;
    bool have_since = false;
    bool have_maximum = false;
    if (!query_unsigned(request, "since", &since, &have_since) ||
        !query_unsigned(request, "max_bytes", &maximum_value, &have_maximum) ||
        maximum_value == 0 || maximum_value > kMaximumLogRead) {
        return send_json_error(request, "400 Bad Request",
                               "invalid since or max_bytes");
    }
    const auto maximum = static_cast<std::size_t>(maximum_value);
    const auto before = self->history_->snapshot();
    if (!have_since) {
        since = before.end - std::min<std::uint64_t>(before.size, maximum);
    }

    auto* body = static_cast<std::uint8_t*>(
        heap_caps_malloc(maximum, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (body == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    std::uint64_t actual_start = 0;
    const auto length =
        self->history_->read(since, body, maximum, &actual_start);
    const auto after = self->history_->snapshot();
    char start_text[24]{};
    char next_text[24]{};
    char oldest_text[24]{};
    char end_text[24]{};
    std::snprintf(start_text, sizeof(start_text), "%" PRIu64, actual_start);
    std::snprintf(next_text, sizeof(next_text), "%" PRIu64,
                  actual_start + length);
    std::snprintf(oldest_text, sizeof(oldest_text), "%" PRIu64, after.start);
    std::snprintf(end_text, sizeof(end_text), "%" PRIu64, after.end);
    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Log-Start", start_text);
    httpd_resp_set_hdr(request, "X-Log-Next", next_text);
    httpd_resp_set_hdr(request, "X-Log-Oldest", oldest_text);
    httpd_resp_set_hdr(request, "X-Log-End", end_text);
    const auto result = httpd_resp_send(
        request, reinterpret_cast<const char*>(body),
        static_cast<std::ptrdiff_t>(length));
    heap_caps_free(body);
    return result;
}

esp_err_t HttpService::uart_write_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    bool interactive = false;
    const auto marker_length =
        httpd_req_get_hdr_value_len(request, "X-STB-Buddy-Interactive");
    if (marker_length == 1) {
        char marker[2]{};
        interactive =
            httpd_req_get_hdr_value_str(request, "X-STB-Buddy-Interactive",
                                        marker, sizeof(marker)) == ESP_OK &&
            marker[0] == '1';
    }
    const auto length = static_cast<std::size_t>(request->content_len);
    if (length == 0 || length > kMaximumUartWrite) {
        return send_json_error(request, "413 Content Too Large",
                               "UART write must be 1..4096 bytes");
    }
    auto* body = static_cast<std::uint8_t*>(
        heap_caps_malloc(length, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (body == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    if (receive_body(request, reinterpret_cast<char*>(body), length) != ESP_OK) {
        heap_caps_free(body);
        return send_json_error(request, "400 Bad Request",
                               "incomplete UART write");
    }
    std::size_t written = 0;
    const auto result = self->uart_->write(body, length, &written);
    if (result != ESP_OK || written != length) {
        heap_caps_free(body);
        return send_json_error(request, "500 Internal Server Error",
                               "UART write failed");
    }
    if (!interactive) {
        self->uart_->record_note("HTTP", body, length);
    }
    heap_caps_free(body);
    char response[48]{};
    std::snprintf(response, sizeof(response), "{\"written\":%u}",
                  static_cast<unsigned>(written));
    return send_json(request, response);
}

esp_err_t HttpService::uart_events_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    std::uint64_t since = 0;
    bool have_since = false;
    if (!query_unsigned(request, "since", &since, &have_since)) {
        return send_json_error(request, "400 Bad Request",
                               "since must be a non-negative integer");
    }

    void* memory = heap_caps_malloc(sizeof(UartNoteBatch),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    auto* batch = new (memory) UartNoteBatch{};
    if (!self->uart_->notes_since(since, batch)) {
        batch->~UartNoteBatch();
        heap_caps_free(batch);
        return send_json_error(request, "500 Internal Server Error",
                               "UART annotation log unavailable");
    }

    auto* response = cJSON_CreateObject();
    auto* events = response == nullptr
                       ? nullptr
                       : cJSON_AddArrayToObject(response, "events");
    if (events == nullptr) {
        cJSON_Delete(response);
        batch->~UartNoteBatch();
        heap_caps_free(batch);
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    bool complete = true;
    for (std::size_t index = 0; index < batch->count; ++index) {
        const auto& event = batch->events[index];
        auto* item = cJSON_CreateObject();
        complete = item != nullptr &&
                   cJSON_AddNumberToObject(
                       item, "sequence",
                       static_cast<double>(event.sequence)) != nullptr &&
                   cJSON_AddNumberToObject(
                       item, "uptime_ms",
                       static_cast<double>(event.uptime_ms)) != nullptr &&
                   cJSON_AddStringToObject(item, "source",
                                           event.source.data()) != nullptr &&
                   cJSON_AddStringToObject(item, "message",
                                           event.message.data()) != nullptr &&
                   cJSON_AddItemToArray(events, item) && complete;
        if (!complete) {
            cJSON_Delete(item);
            break;
        }
    }
    complete = cJSON_AddNumberToObject(
                   response, "oldest",
                   static_cast<double>(batch->oldest)) != nullptr &&
               complete;
    complete = cJSON_AddNumberToObject(
                   response, "next", static_cast<double>(batch->next)) !=
                   nullptr &&
               complete;
    batch->~UartNoteBatch();
    heap_caps_free(batch);
    if (!complete) {
        cJSON_Delete(response);
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    return send_cjson(request, response);
}

esp_err_t HttpService::uart_configure_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    constexpr std::size_t kMaximumBody = 256;
    const auto length = static_cast<std::size_t>(request->content_len);
    if (length == 0 || length > kMaximumBody) {
        return send_json_error(request, "413 Content Too Large",
                               "UART configuration is too large");
    }
    char body[kMaximumBody + 1]{};
    if (receive_body(request, body, length) != ESP_OK) {
        return send_json_error(request, "400 Bad Request",
                               "incomplete UART configuration");
    }
    auto* json = cJSON_ParseWithLength(body, length);
    const auto* baud_json = json == nullptr
                                ? nullptr
                                : cJSON_GetObjectItemCaseSensitive(json,
                                                                  "baud");
    const auto* data_bits_json =
        json == nullptr
            ? nullptr
            : cJSON_GetObjectItemCaseSensitive(json, "data_bits");
    const auto* parity_json = json == nullptr
                                  ? nullptr
                                  : cJSON_GetObjectItemCaseSensitive(json,
                                                                    "parity");
    const auto* stop_bits_json =
        json == nullptr
            ? nullptr
            : cJSON_GetObjectItemCaseSensitive(json, "stop_bits");
    if (!cJSON_IsNumber(baud_json) || !cJSON_IsNumber(data_bits_json) ||
        !cJSON_IsString(parity_json) || !cJSON_IsNumber(stop_bits_json)) {
        cJSON_Delete(json);
        return send_json_error(
            request, "400 Bad Request",
            "baud, data_bits, parity and stop_bits are required");
    }

    const auto baud = baud_json->valuedouble;
    const auto data_bits = data_bits_json->valuedouble;
    const auto stop_bits = stop_bits_json->valuedouble;
    UartParity parity = UartParity::none;
    const auto* parity_text = parity_json->valuestring;
    const bool valid_parity = std::strcmp(parity_text, "none") == 0 ||
                              std::strcmp(parity_text, "even") == 0 ||
                              std::strcmp(parity_text, "odd") == 0;
    if (std::strcmp(parity_text, "even") == 0) {
        parity = UartParity::even;
    } else if (std::strcmp(parity_text, "odd") == 0) {
        parity = UartParity::odd;
    }
    const bool valid_stop_bits = stop_bits == 1.0 || stop_bits == 1.5 ||
                                 stop_bits == 2.0;
    if (!std::isfinite(baud) || std::floor(baud) != baud || baud < 1200 ||
        baud > 3'000'000 || !std::isfinite(data_bits) ||
        std::floor(data_bits) != data_bits || data_bits < 5 || data_bits > 8 ||
        !valid_parity || !std::isfinite(stop_bits) || !valid_stop_bits) {
        cJSON_Delete(json);
        return send_json_error(
            request, "422 Unprocessable Content",
            "UART must be 1200..3000000 baud, 5..8 data bits, "
            "none/even/odd parity and 1/1.5/2 stop bits");
    }
    const UartRuntimeConfig configuration{
        .baud = static_cast<std::uint32_t>(baud),
        .data_bits = static_cast<std::uint8_t>(data_bits),
        .parity = parity,
        .stop_bits_x2 = static_cast<std::uint8_t>(stop_bits * 2),
    };
    cJSON_Delete(json);
    const auto result = self->uart_->configure(configuration);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "configure UART failed: %s", esp_err_to_name(result));
        return send_json_error(request, "500 Internal Server Error",
                               "could not configure UART");
    }

    auto* response = cJSON_CreateObject();
    if (response == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "UART configured; response allocation failed");
    }
    cJSON_AddNumberToObject(response, "port", 1);
    cJSON_AddNumberToObject(response, "baud", configuration.baud);
    cJSON_AddNumberToObject(response, "data_bits", configuration.data_bits);
    cJSON_AddStringToObject(response, "parity",
                            uart_parity_name(configuration.parity));
    cJSON_AddNumberToObject(
        response, "stop_bits",
        static_cast<double>(configuration.stop_bits_x2) / 2.0);
    return send_cjson(request, response);
}

esp_err_t HttpService::clear_history_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    const auto before = self->history_->snapshot();
    const auto next = self->history_->clear();
    const auto cleared_notes = self->uart_->clear_notes();
    char response[128]{};
    std::snprintf(response, sizeof(response),
                  "{\"cleared_bytes\":%u,\"cleared_notes\":%u,"
                  "\"next\":%" PRIu64 "}",
                  static_cast<unsigned>(before.size),
                  static_cast<unsigned>(cleared_notes), next);
    return send_json(request, response);
}

esp_err_t HttpService::log_download_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    const auto target = self->history_->snapshot();
    auto* buffer = static_cast<std::uint8_t*>(
        heap_caps_malloc(kLogChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Content-Disposition",
                       "attachment; filename=\"stb-buddy-uart.log\"");

    auto cursor = target.start;
    auto result = ESP_OK;
    while (result == ESP_OK && cursor < target.end) {
        std::uint64_t actual_start = 0;
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(
            kLogChunkBytes, target.end - cursor));
        const auto length =
            self->history_->read(cursor, buffer, wanted, &actual_start);
        if (actual_start >= target.end || length == 0) {
            break;
        }
        result = httpd_resp_send_chunk(
            request, reinterpret_cast<const char*>(buffer), length);
        cursor = actual_start + length;
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, nullptr, 0);
    }
    heap_caps_free(buffer);
    return result;
}

esp_err_t HttpService::file_download_start_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    constexpr std::size_t kMaximumBody = 512;
    const auto length = static_cast<std::size_t>(request->content_len);
    if (length == 0 || length > kMaximumBody) {
        return send_json_error(request, "413 Content Too Large",
                               "download request is too large");
    }
    char body[kMaximumBody + 1]{};
    if (receive_body(request, body, length) != ESP_OK) {
        return send_json_error(request, "400 Bad Request",
                               "incomplete download request");
    }
    auto* json = cJSON_ParseWithLength(body, length);
    const auto* path_json = json == nullptr
                                ? nullptr
                                : cJSON_GetObjectItemCaseSensitive(json,
                                                                  "path");
    const auto* timeout_json = json == nullptr
                                   ? nullptr
                                   : cJSON_GetObjectItemCaseSensitive(
                                         json, "timeout");
    if (!cJSON_IsString(path_json) ||
        (timeout_json != nullptr && !cJSON_IsNumber(timeout_json))) {
        cJSON_Delete(json);
        return send_json_error(request, "400 Bad Request",
                               "path string and optional timeout are required");
    }
    std::uint32_t timeout = kMaximumDownloadSeconds;
    if (timeout_json != nullptr) {
        const auto value = timeout_json->valuedouble;
        if (!std::isfinite(value) || std::floor(value) != value || value < 1 ||
            value > kMaximumDownloadSeconds) {
            cJSON_Delete(json);
            return send_json_error(request, "400 Bad Request",
                                   "timeout must be 1..600 seconds");
        }
        timeout = static_cast<std::uint32_t>(value);
    }
    std::uint32_t generation = 0;
    const auto result =
        self->downloads_->start(path_json->valuestring, timeout, &generation);
    cJSON_Delete(json);
    if (result == DownloadStartResult::busy) {
        return send_json_error(request, "409 Conflict",
                               "another file download is running");
    }
    if (result == DownloadStartResult::invalid) {
        return send_json_error(request, "400 Bad Request",
                               "invalid path or timeout");
    }
    if (result != DownloadStartResult::started) {
        return send_json_error(request, "500 Internal Server Error",
                               "could not start file download");
    }
    char response[72]{};
    std::snprintf(response, sizeof(response),
                  "{\"started\":true,\"generation\":%lu}",
                  static_cast<unsigned long>(generation));
    httpd_resp_set_status(request, "202 Accepted");
    return send_json(request, response);
}

esp_err_t HttpService::file_download_status_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    return send_cjson(request,
                      download_snapshot_json(self->downloads_->snapshot()));
}

esp_err_t HttpService::downloaded_file_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    constexpr char prefix[] = "/api/downloads/";
    if (std::strncmp(request->uri, prefix, sizeof(prefix) - 1) != 0) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND,
                                   "download not found");
    }
    const char* name = request->uri + sizeof(prefix) - 1;
    if (name[0] == '\0' || std::strchr(name, '/') != nullptr ||
        std::strchr(name, '?') != nullptr) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND,
                                   "download not found");
    }
    auto* buffer = static_cast<std::uint8_t*>(heap_caps_malloc(
        kLogChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    std::size_t total = 0;
    bool found = false;
    auto count = self->downloads_->read_file(
        name, 0, buffer, kLogChunkBytes, &total, &found);
    if (!found) {
        heap_caps_free(buffer);
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND,
                                   "download not found");
    }
    char disposition[128]{};
    std::snprintf(disposition, sizeof(disposition),
                  "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Content-Disposition", disposition);
    std::size_t offset = 0;
    auto result = ESP_OK;
    while (result == ESP_OK && offset < total) {
        if (offset != 0) {
            count = self->downloads_->read_file(
                name, offset, buffer, kLogChunkBytes, &total, &found);
        }
        if (!found || count == 0) {
            result = ESP_FAIL;
            break;
        }
        result = httpd_resp_send_chunk(
            request, reinterpret_cast<const char*>(buffer), count);
        offset += count;
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, nullptr, 0);
    }
    heap_caps_free(buffer);
    return result;
}

esp_err_t HttpService::wifi_status_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    const auto wifi = self->wifi_->snapshot();
    auto* response = cJSON_CreateObject();
    if (response == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    cJSON_AddStringToObject(response, "mode",
                            wifi.setup_mode ? "setup-ap" : "station");
    cJSON_AddBoolToObject(response, "connected", wifi.connected);
    cJSON_AddStringToObject(response, "ssid", wifi.ssid.data());
    cJSON_AddStringToObject(response, "ip", wifi.ip.data());
    return send_cjson(request, response);
}

esp_err_t HttpService::wifi_save_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    if (!self->wifi_->setup_mode()) {
        return send_json_error(request, "409 Conflict",
                               "enter Wi-Fi setup mode with the button first");
    }
    constexpr std::size_t kMaximumBody = 768;
    const auto length = static_cast<std::size_t>(request->content_len);
    if (length == 0 || length > kMaximumBody) {
        return send_json_error(request, "413 Content Too Large",
                               "Wi-Fi request is too large");
    }
    char body[kMaximumBody + 1]{};
    if (receive_body(request, body, length) != ESP_OK) {
        return send_json_error(request, "400 Bad Request",
                               "incomplete Wi-Fi request");
    }
    cJSON* json = cJSON_ParseWithLength(body, length);
    const auto* ssid_json =
        json == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const auto* password_json = json == nullptr
                                    ? nullptr
                                    : cJSON_GetObjectItemCaseSensitive(
                                          json, "password");
    const char* ssid = cJSON_IsString(ssid_json) ? ssid_json->valuestring : nullptr;
    const char* password =
        cJSON_IsString(password_json) ? password_json->valuestring : nullptr;
    if (ssid == nullptr || password == nullptr) {
        cJSON_Delete(json);
        return send_json_error(request, "400 Bad Request",
                               "ssid and password strings are required");
    }
    const auto result = self->wifi_->save_credentials(ssid, password);
    cJSON_Delete(json);
    if (result == ESP_ERR_INVALID_ARG) {
        return send_json_error(
            request, "422 Unprocessable Content",
            "SSID must be 1..32 bytes; password must be empty or 8..63 bytes");
    }
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "save Wi-Fi credentials failed: %s",
                 esp_err_to_name(result));
        return send_json_error(request, "500 Internal Server Error",
                               "could not save Wi-Fi credentials");
    }
    if (xTaskCreate(delayed_restart, "wifi_restart", 2048, nullptr, 5,
                    nullptr) != pdPASS) {
        return send_json_error(request, "500 Internal Server Error",
                               "credentials saved; restart failed");
    }
    return send_json(request, "{\"saved\":true,\"restarting\":true}");
}

esp_err_t HttpService::remotes_upload_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    const auto length = static_cast<std::size_t>(request->content_len);
    if (length == 0 || length > kLircdMaxConfigBytes) {
        return send_json_error(request, "413 Content Too Large",
                               "lircd.conf must be 1..8192 bytes");
    }
    auto* body = static_cast<char*>(heap_caps_malloc(
        length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (body == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    const auto receive_result = receive_body(request, body, length);
    if (receive_result != ESP_OK) {
        heap_caps_free(body);
        return send_json_error(request, "400 Bad Request",
                               "incomplete request body");
    }

    LircdParseError error{};
    const auto result =
        self->remotes_->load(std::string_view{body, length}, &error);
    heap_caps_free(body);
    if (result == ESP_ERR_INVALID_ARG) {
        char response[192]{};
        std::snprintf(response, sizeof(response),
                      "{\"error\":\"%s\",\"line\":%u}",
                      error.message.data(),
                      static_cast<unsigned>(error.line));
        httpd_resp_set_status(request, "422 Unprocessable Content");
        return send_json(request, response);
    }
    if (result != ESP_OK) {
        return send_json_error(request, "500 Internal Server Error",
                               "could not persist configuration");
    }

    char response[128]{};
    std::snprintf(response, sizeof(response),
                  "{\"loaded\":true,\"remote_count\":%u,"
                  "\"key_count\":%u}",
                  static_cast<unsigned>(self->remotes_->remote_count()),
                  static_cast<unsigned>(self->remotes_->key_count()));
    return send_json(request, response);
}

esp_err_t HttpService::remotes_download_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    auto* body = static_cast<char*>(heap_caps_malloc(
        kLircdMaxConfigBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (body == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    std::size_t length = kLircdMaxConfigBytes;
    const auto result = self->remotes_->read_configuration(body, &length);
    if (result == ESP_ERR_NOT_FOUND) {
        heap_caps_free(body);
        return send_json_error(request, "404 Not Found",
                               "no lircd.conf is stored");
    }
    if (result != ESP_OK || length == 0 || length > kLircdMaxConfigBytes) {
        heap_caps_free(body);
        ESP_LOGE(kTag, "read lircd.conf failed: %s", esp_err_to_name(result));
        return send_json_error(request, "500 Internal Server Error",
                               "could not read configuration");
    }

    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Content-Disposition",
                       "attachment; filename=\"lircd.conf\"");
    const auto send_result = httpd_resp_send(
        request, body, static_cast<std::ptrdiff_t>(length));
    heap_caps_free(body);
    return send_result;
}

esp_err_t HttpService::remotes_list_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    void* memory = heap_caps_malloc(sizeof(LircdDatabase),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    auto* database = new (memory) LircdDatabase{};
    if (!self->remotes_->snapshot(database)) {
        database->~LircdDatabase();
        heap_caps_free(database);
        return send_json_error(request, "500 Internal Server Error",
                               "remote store unavailable");
    }

    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char chunk[160]{};
    auto result = httpd_resp_send_chunk(request, "{\"remotes\":[", 12);
    for (std::size_t remote_index = 0;
         result == ESP_OK && remote_index < database->remote_count;
         ++remote_index) {
        const auto& remote = database->remotes[remote_index];
        const auto count = std::snprintf(
            chunk, sizeof(chunk), "%s{\"index\":%u,\"name\":",
            remote_index == 0 ? "" : ",",
            static_cast<unsigned>(remote_index));
        result = httpd_resp_send_chunk(request, chunk, count);
        if (result == ESP_OK) {
            result = send_escaped_json_string(request, remote.name.data());
        }
        if (result == ESP_OK) {
            const auto suffix = std::snprintf(
                chunk, sizeof(chunk), ",\"protocol\":\"%s\",\"keys\":[",
                protocol_name(remote.protocol));
            result = httpd_resp_send_chunk(request, chunk, suffix);
        }
        for (std::size_t key_offset = 0;
             result == ESP_OK && key_offset < remote.key_count; ++key_offset) {
            if (key_offset != 0) {
                result = httpd_resp_send_chunk(request, ",", 1);
            }
            if (result == ESP_OK) {
                const auto& key =
                    database->keys[remote.first_key + key_offset];
                result = send_escaped_json_string(request, key.name.data());
            }
        }
        if (result == ESP_OK) {
            result = httpd_resp_send_chunk(request, "]}", 2);
        }
    }
    if (result == ESP_OK) {
        const auto count = std::snprintf(
            chunk, sizeof(chunk),
            "],\"remote_count\":%u,\"key_count\":%u}",
            static_cast<unsigned>(database->remote_count),
            static_cast<unsigned>(database->key_count));
        result = httpd_resp_send_chunk(request, chunk, count);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, nullptr, 0);
    }
    database->~LircdDatabase();
    heap_caps_free(database);
    return result;
}

esp_err_t HttpService::ir_send_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    constexpr std::size_t kMaximumBody = 512;
    const auto length = static_cast<std::size_t>(request->content_len);
    if (length == 0 || length > kMaximumBody) {
        return send_json_error(request, "413 Content Too Large",
                               "IR request must be 1..512 bytes");
    }
    char body[kMaximumBody + 1]{};
    if (receive_body(request, body, length) != ESP_OK) {
        return send_json_error(request, "400 Bad Request",
                               "incomplete request body");
    }
    cJSON* json = cJSON_ParseWithLength(body, length);
    if (json == nullptr) {
        return send_json_error(request, "400 Bad Request", "invalid JSON");
    }

    const cJSON* remote_json =
        cJSON_GetObjectItemCaseSensitive(json, "remote");
    const cJSON* key_json = cJSON_GetObjectItemCaseSensitive(json, "key");
    const cJSON* repeats_json =
        cJSON_GetObjectItemCaseSensitive(json, "repeats");
    bool by_index = false;
    std::size_t remote_index = 0;
    const char* remote_name = nullptr;
    const char* key_name = cJSON_IsString(key_json) ? key_json->valuestring
                                                    : nullptr;
    unsigned repeats = 0;
    bool valid = key_name != nullptr && *key_name != '\0';
    if (cJSON_IsNumber(remote_json) && remote_json->valuedouble >= 0 &&
        remote_json->valuedouble < kLircdMaxRemotes &&
        std::floor(remote_json->valuedouble) == remote_json->valuedouble) {
        by_index = true;
        remote_index = static_cast<std::size_t>(remote_json->valuedouble);
    } else if (cJSON_IsString(remote_json) &&
               remote_json->valuestring[0] != '\0') {
        remote_name = remote_json->valuestring;
    } else {
        valid = false;
    }
    if (repeats_json != nullptr) {
        if (!cJSON_IsNumber(repeats_json) || repeats_json->valuedouble < 0 ||
            repeats_json->valuedouble > 10 ||
            std::floor(repeats_json->valuedouble) !=
                repeats_json->valuedouble) {
            valid = false;
        } else {
            repeats = static_cast<unsigned>(repeats_json->valuedouble);
        }
    }
    if (!valid) {
        cJSON_Delete(json);
        return send_json_error(
            request, "400 Bad Request",
            "expected remote index/name, key name and repeats 0..10");
    }

    LircdRemote remote{};
    std::uint64_t code = 0;
    const auto resolve_result = self->remotes_->resolve_key(
        by_index, remote_index, remote_name, key_name, &remote, &code);
    if (resolve_result != ESP_OK) {
        cJSON_Delete(json);
        return send_json_error(request, "404 Not Found",
                               "remote or key not found");
    }
    const auto send_result = self->ir_->send(remote, code, repeats, key_name);
    if (send_result != ESP_OK) {
        cJSON_Delete(json);
        return send_json_error(request, "500 Internal Server Error",
                               "IR transmission failed");
    }

    char response[64]{};
    std::snprintf(response, sizeof(response),
                  "{\"sent\":true,\"repeats\":%u}", repeats);
    cJSON_Delete(json);
    return send_json(request, response);
}

esp_err_t HttpService::ir_events_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    std::uint64_t since = 0;
    bool have_since = false;
    if (!query_unsigned(request, "since", &since, &have_since)) {
        return send_json_error(request, "400 Bad Request",
                               "since must be a non-negative integer");
    }

    void* memory = heap_caps_malloc(sizeof(IrEventBatch),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    auto* batch = new (memory) IrEventBatch{};
    if (!self->ir_->events_since(since, batch)) {
        batch->~IrEventBatch();
        heap_caps_free(batch);
        return send_json_error(request, "500 Internal Server Error",
                               "IR event log unavailable");
    }

    auto* response = cJSON_CreateObject();
    auto* events = response == nullptr
                       ? nullptr
                       : cJSON_AddArrayToObject(response, "events");
    if (events == nullptr) {
        cJSON_Delete(response);
        batch->~IrEventBatch();
        heap_caps_free(batch);
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    bool complete = true;
    for (std::size_t index = 0; index < batch->count; ++index) {
        const auto& event = batch->events[index];
        auto* item = cJSON_CreateObject();
        complete = item != nullptr &&
                   cJSON_AddNumberToObject(
                       item, "sequence",
                       static_cast<double>(event.sequence)) != nullptr &&
                   cJSON_AddNumberToObject(
                       item, "uptime_ms",
                       static_cast<double>(event.uptime_ms)) != nullptr &&
                   cJSON_AddStringToObject(item, "message",
                                           event.message.data()) != nullptr &&
                   cJSON_AddItemToArray(events, item) && complete;
        if (!complete) {
            cJSON_Delete(item);
            break;
        }
    }
    complete = cJSON_AddNumberToObject(
                   response, "oldest",
                   static_cast<double>(batch->oldest)) != nullptr &&
               complete;
    complete = cJSON_AddNumberToObject(
                   response, "next", static_cast<double>(batch->next)) !=
                   nullptr &&
               complete;
    batch->~IrEventBatch();
    heap_caps_free(batch);
    if (!complete) {
        cJSON_Delete(response);
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    return send_cjson(request, response);
}

esp_err_t HttpService::ota_status_handler(httpd_req_t* request) {
    const auto* running = esp_ota_get_running_partition();
    const auto* boot = esp_ota_get_boot_partition();

    auto* response = cJSON_CreateObject();
    if (response == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }
    if (!add_partition_json(response, "running", running) ||
        !add_partition_json(response, "boot", boot) ||
        cJSON_AddBoolToObject(
            response, "reboot_required",
            running != nullptr && boot != nullptr &&
                running->address != boot->address) == nullptr) {
        cJSON_Delete(response);
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running != nullptr &&
        esp_ota_get_state_partition(running, &state) == ESP_OK) {
        cJSON_AddStringToObject(response, "running_state",
                                ota_state_name(state));
    } else {
        cJSON_AddStringToObject(response, "running_state", "unavailable");
    }
    return send_cjson(request, response);
}

esp_err_t HttpService::ota_upload_handler(httpd_req_t* request) {
    const auto* update_partition = esp_ota_get_next_update_partition(nullptr);
    if (update_partition == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "no inactive OTA partition");
    }

    const auto length = static_cast<std::size_t>(request->content_len);
    if (length == 0 || length > update_partition->size) {
        return send_json_error(request, "413 Content Too Large",
                               "image must fit the inactive OTA partition");
    }

    auto* buffer = static_cast<char*>(heap_caps_malloc(
        kOtaBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "out of memory");
    }

    esp_ota_handle_t handle = 0;
    auto result = esp_ota_begin(update_partition, length, &handle);
    if (result != ESP_OK) {
        heap_caps_free(buffer);
        ESP_LOGE(kTag, "OTA begin failed: %s", esp_err_to_name(result));
        return send_json_error(request, "500 Internal Server Error",
                               "could not start OTA update");
    }

    std::size_t received = 0;
    while (received < length) {
        const auto chunk = std::min(
            kOtaBufferBytes, static_cast<std::size_t>(length - received));
        result = receive_body(request, buffer, chunk);
        if (result != ESP_OK) {
            esp_ota_abort(handle);
            heap_caps_free(buffer);
            ESP_LOGW(kTag, "OTA upload interrupted after %u of %u bytes",
                     static_cast<unsigned>(received),
                     static_cast<unsigned>(length));
            return send_json_error(request, "400 Bad Request",
                                   "incomplete firmware image");
        }
        result = esp_ota_write(handle, buffer, chunk);
        if (result != ESP_OK) {
            esp_ota_abort(handle);
            heap_caps_free(buffer);
            ESP_LOGE(kTag, "OTA write failed: %s", esp_err_to_name(result));
            return send_json_error(request, "422 Unprocessable Content",
                                   "invalid firmware image");
        }
        received += chunk;
    }
    heap_caps_free(buffer);

    result = esp_ota_end(handle);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "OTA validation failed: %s", esp_err_to_name(result));
        return send_json_error(request, "422 Unprocessable Content",
                               "firmware validation failed");
    }

    esp_app_desc_t description{};
    result = esp_ota_get_partition_description(update_partition, &description);
    if (result != ESP_OK ||
        std::strncmp(description.project_name, "stb_buddy",
                     sizeof(description.project_name)) != 0) {
        ESP_LOGW(kTag, "rejected OTA image for project '%.*s'",
                 static_cast<int>(sizeof(description.project_name)),
                 description.project_name);
        return send_json_error(request, "422 Unprocessable Content",
                               "firmware project must be stb_buddy");
    }

    result = esp_ota_set_boot_partition(update_partition);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "select OTA boot partition failed: %s",
                 esp_err_to_name(result));
        return send_json_error(request, "500 Internal Server Error",
                               "could not select firmware for boot");
    }

    ESP_LOGI(kTag, "OTA image %s (%u bytes) ready in %s",
             description.version, static_cast<unsigned>(length),
             update_partition->label);
    auto* response = cJSON_CreateObject();
    if (response == nullptr) {
        return send_json_error(request, "500 Internal Server Error",
                               "firmware installed; status response failed");
    }
    cJSON_AddBoolToObject(response, "installed", true);
    cJSON_AddStringToObject(response, "version", description.version);
    cJSON_AddStringToObject(response, "partition", update_partition->label);
    cJSON_AddBoolToObject(response, "reboot_required", true);
    return send_cjson(request, response);
}

esp_err_t HttpService::ota_reboot_handler(httpd_req_t* request) {
    TaskHandle_t task = nullptr;
    if (xTaskCreate(delayed_restart, "ota_restart", 2048, nullptr, 5, &task) !=
        pdPASS) {
        return send_json_error(request, "500 Internal Server Error",
                               "could not schedule restart");
    }
    return send_json(request, "{\"restarting\":true}");
}

esp_err_t HttpService::mcp_handler(httpd_req_t* request) {
    auto* self = static_cast<HttpService*>(request->user_ctx);
    if (xSemaphoreTake(self->mcp_worker_slots_, 0) != pdTRUE) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_hdr(request, "Retry-After", "1");
        return httpd_resp_sendstr(request, "all MCP workers are busy");
    }

    httpd_req_t* asynchronous = nullptr;
    auto result = httpd_req_async_handler_begin(request, &asynchronous);
    if (result != ESP_OK) {
        xSemaphoreGive(self->mcp_worker_slots_);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "could not create asynchronous request");
    }

    if (xTaskCreate(mcp_task_entry, "mcp_request", kMcpWorkerStackBytes,
                    asynchronous, 5, nullptr) != pdPASS) {
        httpd_resp_set_status(asynchronous, "503 Service Unavailable");
        httpd_resp_set_hdr(asynchronous, "Retry-After", "1");
        httpd_resp_sendstr(asynchronous, "could not start MCP worker");
        httpd_req_async_handler_complete(asynchronous);
        xSemaphoreGive(self->mcp_worker_slots_);
    }
    return ESP_OK;
}

void HttpService::mcp_task_entry(void* context) {
    auto* request = static_cast<httpd_req_t*>(context);
    auto* self = static_cast<HttpService*>(request->user_ctx);
    const auto result = self->mcp_->handle(request);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "asynchronous MCP request failed: %s",
                 esp_err_to_name(result));
    }
    if (httpd_req_async_handler_complete(request) != ESP_OK) {
        ESP_LOGW(kTag, "could not complete asynchronous MCP request");
    }
    xSemaphoreGive(self->mcp_worker_slots_);
    vTaskDelete(nullptr);
}

}  // namespace stb_buddy
