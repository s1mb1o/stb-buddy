#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "file_download.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ir_blaster.hpp"
#include "mcp_service.hpp"
#include "remote_store.hpp"
#include "ring_log.hpp"
#include "uart_console.hpp"
#include "wifi_service.hpp"

namespace stb_buddy {

class HttpService final {
public:
    esp_err_t start(RingLog* history, UartConsole* uart, IrBlaster* ir,
                    WifiService* wifi, RemoteStore* remotes, McpService* mcp,
                    FileDownloadService* downloads);

private:
    static esp_err_t index_handler(httpd_req_t* request);
    static esp_err_t docs_handler(httpd_req_t* request);
    static esp_err_t openapi_handler(httpd_req_t* request);
    static esp_err_t static_asset_handler(httpd_req_t* request);
    static esp_err_t health_handler(httpd_req_t* request);
    static esp_err_t log_read_handler(httpd_req_t* request);
    static esp_err_t uart_write_handler(httpd_req_t* request);
    static esp_err_t uart_events_handler(httpd_req_t* request);
    static esp_err_t uart_configure_handler(httpd_req_t* request);
    static esp_err_t clear_history_handler(httpd_req_t* request);
    static esp_err_t log_download_handler(httpd_req_t* request);
    static esp_err_t file_download_start_handler(httpd_req_t* request);
    static esp_err_t file_download_status_handler(httpd_req_t* request);
    static esp_err_t downloaded_file_handler(httpd_req_t* request);
    static esp_err_t wifi_status_handler(httpd_req_t* request);
    static esp_err_t wifi_save_handler(httpd_req_t* request);
    static esp_err_t remotes_upload_handler(httpd_req_t* request);
    static esp_err_t remotes_download_handler(httpd_req_t* request);
    static esp_err_t remotes_list_handler(httpd_req_t* request);
    static esp_err_t ir_send_handler(httpd_req_t* request);
    static esp_err_t ir_events_handler(httpd_req_t* request);
    static esp_err_t ota_status_handler(httpd_req_t* request);
    static esp_err_t ota_upload_handler(httpd_req_t* request);
    static esp_err_t ota_reboot_handler(httpd_req_t* request);
    static esp_err_t mcp_handler(httpd_req_t* request);
    static void mcp_task_entry(void* context);

    httpd_handle_t server_{nullptr};
    RingLog* history_{nullptr};
    UartConsole* uart_{nullptr};
    IrBlaster* ir_{nullptr};
    WifiService* wifi_{nullptr};
    RemoteStore* remotes_{nullptr};
    McpService* mcp_{nullptr};
    FileDownloadService* downloads_{nullptr};
    SemaphoreHandle_t mcp_worker_slots_{nullptr};
};

}  // namespace stb_buddy
