#pragma once

#include <cstddef>
#include <cstdint>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "file_download.hpp"
#include "ir_blaster.hpp"
#include "remote_store.hpp"
#include "ring_log.hpp"
#include "uart_console.hpp"
#include "wifi_service.hpp"

namespace stb_buddy {

class McpService final {
public:
    esp_err_t init(RingLog* history, UartConsole* uart, IrBlaster* ir,
                   WifiService* wifi, RemoteStore* remotes,
                   FileDownloadService* downloads);
    esp_err_t handle(httpd_req_t* request);

private:
    cJSON* dispatch_tool(const char* name, const cJSON* arguments,
                         bool* is_error);
    cJSON* tool_status();
    cJSON* tool_read(const cJSON* arguments, bool* is_error);
    cJSON* tool_write(const cJSON* arguments, bool* is_error);
    cJSON* tool_wait_for(const cJSON* arguments, bool* is_error);
    cJSON* tool_send_and_wait(const cJSON* arguments, bool* is_error);
    cJSON* tool_clear_history();
    cJSON* tool_uart_configure(const cJSON* arguments, bool* is_error);
    cJSON* tool_remotes_load(const cJSON* arguments, bool* is_error);
    cJSON* tool_remotes_list();
    cJSON* tool_ir_send_key(const cJSON* arguments, bool* is_error);
    cJSON* tool_ir_send_nec(const cJSON* arguments, bool* is_error);
    cJSON* tool_ir_send_raw(const cJSON* arguments, bool* is_error);
    cJSON* tool_download(const cJSON* arguments, bool* is_error);

    cJSON* wait_for_pattern(const char* pattern, std::uint64_t since,
                            std::uint32_t timeout_ms, const char* reply,
                            bool* is_error);

    RingLog* history_{nullptr};
    UartConsole* uart_{nullptr};
    IrBlaster* ir_{nullptr};
    WifiService* wifi_{nullptr};
    RemoteStore* remotes_{nullptr};
    FileDownloadService* downloads_{nullptr};
};

}  // namespace stb_buddy
