#include "display_service.hpp"
#include "file_download.hpp"
#include "http_service.hpp"
#include "ir_blaster.hpp"
#include "mcp_service.hpp"
#include "remote_store.hpp"
#include "ring_log.hpp"
#include "uart_console.hpp"
#include "wifi_service.hpp"

#include <cstdlib>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

constexpr auto kTag = "stb-buddy";

stb_buddy::RingLog history;
stb_buddy::DisplayService display;
stb_buddy::FileDownloadService downloads;
stb_buddy::UartConsole uart;
stb_buddy::IrBlaster ir;
stb_buddy::WifiService wifi;
stb_buddy::RemoteStore remotes;
stb_buddy::McpService mcp;
stb_buddy::HttpService http;

esp_err_t initialize_nvs() {
    auto result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), kTag, "erase incompatible NVS");
        result = nvs_flash_init();
    }
    return result;
}

esp_err_t confirm_running_image() {
    const auto* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const auto result = esp_ota_get_state_partition(running, &state);
    if (result == ESP_ERR_NOT_SUPPORTED || result == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(result, kTag, "read running OTA image state");
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(kTag, "all services started; confirming OTA image");
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    return ESP_OK;
}

}  // namespace

extern "C" void app_main() {
    ESP_LOGI(kTag, "starting standalone AtomS3R UART/MCP/IR appliance");
    ESP_ERROR_CHECK(initialize_nvs());

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(kTag, "PSRAM is not initialized");
        std::abort();
    }
    ESP_LOGI(kTag, "PSRAM size: %u bytes",
             static_cast<unsigned>(esp_psram_get_size()));

    ESP_ERROR_CHECK(history.init(CONFIG_STB_BUDDY_LOG_BYTES));
    ESP_ERROR_CHECK(remotes.init());
    ESP_ERROR_CHECK(uart.init(&history));
    ESP_ERROR_CHECK(ir.init());
    ESP_ERROR_CHECK(wifi.start());
    ESP_ERROR_CHECK(downloads.init(&history, &uart, &wifi));
    ESP_ERROR_CHECK(display.start(&wifi));
    ESP_ERROR_CHECK(
        mcp.init(&history, &uart, &ir, &wifi, &remotes, &downloads));
    ESP_ERROR_CHECK(http.start(&history, &uart, &ir, &wifi, &remotes, &mcp,
                               &downloads));
    ESP_ERROR_CHECK(confirm_running_image());

    const auto network = wifi.snapshot();
    ESP_LOGI(kTag, "ready; Wi-Fi %s on %s; HTTP port 80",
             network.ssid.data(),
             network.ip[0] == '\0' ? "waiting for an IP address"
                                    : network.ip.data());
}
