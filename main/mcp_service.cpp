#include "mcp_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <regex.h>
#include <string_view>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace stb_buddy {

namespace {

constexpr auto kTag = "mcp";
constexpr auto kModernVersion = "2026-07-28";
constexpr auto kLegacyVersion = "2025-11-25";
constexpr std::size_t kMaximumRequestBytes = 24 * 1024;
constexpr std::size_t kMaximumReadBytes = 64 * 1024;
constexpr std::size_t kMaximumWriteBytes = 4096;
constexpr std::uint32_t kMaximumWaitMs = 60'000;

constexpr char kInstructions[] =
    "Controls one directly connected set-top box. UART offsets are monotonic; "
    "pass a returned next offset to later reads. Use send_and_wait for shell "
    "commands. Load a complete lircd.conf before sending named IR keys. UART "
    "and IR actions affect physical hardware. download requires a Linux shell "
    "prompt and returns a URL for verified file bytes.";

constexpr char kToolsJson[] = R"tools([
{"name":"status","title":"STB Buddy status","description":"Return Wi-Fi, UART, retained-history, heap, remote database, IR and file-download status.","inputSchema":{"type":"object","additionalProperties":false},"outputSchema":{"type":"object","properties":{"name":{"type":"string"},"version":{"type":"string"},"wifi":{"type":"object"},"uart":{"type":"object"},"history":{"type":"object"},"memory":{"type":"object"},"ir":{"type":"object"},"download":{"type":"object"}},"required":["name","version","wifi","uart","history","memory","ir","download"]},"annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"read","title":"Read UART history","description":"Read retained UART bytes. Omit since for a bounded tail; pass returned next to continue. text is UTF-8-safe and data_base64 preserves exact bytes.","inputSchema":{"type":"object","properties":{"since":{"type":"integer","minimum":0},"max_bytes":{"type":"integer","minimum":1,"maximum":65536,"default":32768}},"additionalProperties":false},"outputSchema":{"type":"object","properties":{"text":{"type":"string"},"data_base64":{"type":"string"},"start":{"type":"integer"},"next":{"type":"integer"}},"required":["text","data_base64","start","next"]},"annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"write","title":"Write STB UART","description":"Write UTF-8 text to the STB UART. newline=true appends carriage return (Enter). The bytes can invoke destructive commands on the connected STB. Use the returned offset to read the response. A successful write creates a separate browser annotation.","inputSchema":{"type":"object","properties":{"data":{"type":"string","maxLength":4096},"newline":{"type":"boolean","default":true}},"required":["data"],"additionalProperties":false},"outputSchema":{"type":"object","properties":{"written":{"type":"integer"},"offset":{"type":"integer"}},"required":["written","offset"]},"annotations":{"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"wait_for","title":"Wait for UART pattern","description":"Wait for a POSIX extended regular expression in UART output. Omitted since starts at the current end. Optional reply is sent immediately and exactly, without Enter, under the transaction lock; it can trigger destructive behavior on the connected STB and creates a separate browser annotation.","inputSchema":{"type":"object","properties":{"pattern":{"type":"string","minLength":1,"maxLength":256},"since":{"type":"integer","minimum":0},"timeout_ms":{"type":"integer","minimum":0,"maximum":60000,"default":10000},"reply":{"type":"string","maxLength":4096}},"required":["pattern"],"additionalProperties":false},"outputSchema":{"type":"object","properties":{"matched":{"type":"boolean"},"match":{"type":["string","null"]},"text":{"type":"string"},"data_base64":{"type":"string"},"start":{"type":"integer"},"next":{"type":"integer"}},"required":["matched","match","text","data_base64","start","next"]},"annotations":{"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"send_and_wait","title":"Send command and wait","description":"Serialize a shell transaction: write a potentially destructive command plus Enter, then wait for a POSIX extended regular expression. The command creates a separate browser annotation. Optional reply is sent immediately and exactly after a match and is annotated separately.","inputSchema":{"type":"object","properties":{"command":{"type":"string","minLength":1,"maxLength":4096},"pattern":{"type":"string","minLength":1,"maxLength":256,"default":"# "},"timeout_ms":{"type":"integer","minimum":0,"maximum":60000,"default":10000},"reply":{"type":"string","maxLength":4096}},"required":["command"],"additionalProperties":false},"outputSchema":{"type":"object","properties":{"matched":{"type":"boolean"},"match":{"type":["string","null"]},"text":{"type":"string"},"data_base64":{"type":"string"},"start":{"type":"integer"},"next":{"type":"integer"}},"required":["matched","match","text","data_base64","start","next"]},"annotations":{"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"clear_history","title":"Clear UART history","description":"Destructively discard all retained UART bytes and UART-write annotations. Absolute byte offsets continue increasing and cleared data cannot be read by any client.","inputSchema":{"type":"object","additionalProperties":false},"outputSchema":{"type":"object","properties":{"cleared_bytes":{"type":"integer"},"cleared_notes":{"type":"integer"},"next":{"type":"integer"}},"required":["cleared_bytes","cleared_notes","next"]},"annotations":{"readOnlyHint":false,"destructiveHint":true,"idempotentHint":true,"openWorldHint":false}},
{"name":"uart_configure","title":"Configure STB UART","description":"Change the live UART format. The setting lasts until reboot; reboot restores the firmware default.","inputSchema":{"type":"object","properties":{"baud":{"type":"integer","minimum":1200,"maximum":3000000},"data_bits":{"type":"integer","minimum":5,"maximum":8},"parity":{"type":"string","enum":["none","even","odd"]},"stop_bits":{"type":"number","enum":[1,1.5,2]}},"required":["baud","data_bits","parity","stop_bits"],"additionalProperties":false},"outputSchema":{"type":"object","properties":{"baud":{"type":"integer"},"data_bits":{"type":"integer"},"parity":{"type":"string"},"stop_bits":{"type":"number"}},"required":["baud","data_bits","parity","stop_bits"]},"annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"remotes_load","title":"Load lircd.conf","description":"Validate, activate and persist a complete supported lircd.conf document. Failure leaves the previous remote database active.","inputSchema":{"type":"object","properties":{"config":{"type":"string","minLength":1,"maxLength":8192}},"required":["config"],"additionalProperties":false},"outputSchema":{"type":"object","properties":{"loaded":{"type":"boolean"},"remote_count":{"type":"integer"},"key_count":{"type":"integer"}},"required":["loaded","remote_count","key_count"]},"annotations":{"readOnlyHint":false,"destructiveHint":true,"idempotentHint":true,"openWorldHint":false}},
{"name":"remotes_list","title":"List remote keys","description":"List loaded LIRC remote indexes, names, protocols and named keys.","inputSchema":{"type":"object","additionalProperties":false},"outputSchema":{"type":"object","properties":{"remotes":{"type":"array"},"remote_count":{"type":"integer"},"key_count":{"type":"integer"}},"required":["remotes","remote_count","key_count"]},"annotations":{"readOnlyHint":true,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"ir_send_key","title":"Send named IR key","description":"Resolve and transmit a named key from the persisted LIRC database. remote may be its stable index or case-insensitive name. The target device can interpret the key as a destructive action.","inputSchema":{"type":"object","properties":{"remote":{"oneOf":[{"type":"integer","minimum":0,"maximum":7},{"type":"string","minLength":1,"maxLength":47}]},"key":{"type":"string","minLength":1,"maxLength":47},"repeats":{"type":"integer","minimum":0,"maximum":10,"default":0}},"required":["remote","key"],"additionalProperties":false},"outputSchema":{"type":"object","properties":{"sent":{"type":"boolean"},"key":{"type":"string"},"repeats":{"type":"integer"}},"required":["sent","key","repeats"]},"annotations":{"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"ir_send_nec","title":"Send NEC IR command","description":"Transmit a standard or extended-address NEC command through the onboard IR LED. The target device can interpret the command as a destructive action.","inputSchema":{"type":"object","properties":{"address":{"type":"integer","minimum":0,"maximum":65535},"command":{"type":"integer","minimum":0,"maximum":255},"extended_address":{"type":"boolean","default":false},"repeats":{"type":"integer","minimum":0,"maximum":10,"default":0}},"required":["address","command"],"additionalProperties":false},"outputSchema":{"type":"object","properties":{"sent":{"type":"boolean"},"address":{"type":"integer"},"command":{"type":"integer"},"extended_address":{"type":"boolean"},"repeats":{"type":"integer"}},"required":["sent","address","command","extended_address","repeats"]},"annotations":{"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"ir_send_raw","title":"Send raw IR waveform","description":"Transmit bounded alternating mark/space durations, starting with a mark. This acts on physical hardware and the target can interpret it as a destructive action.","inputSchema":{"type":"object","properties":{"carrier_hz":{"type":"integer","minimum":20000,"maximum":60000},"duty_percent":{"type":"integer","minimum":1,"maximum":99},"durations_us":{"type":"array","minItems":1,"maxItems":256,"items":{"type":"integer","minimum":1,"maximum":32767}},"gap_us":{"type":"integer","minimum":0,"maximum":1000000,"default":0},"repeats":{"type":"integer","minimum":0,"maximum":10,"default":0}},"required":["carrier_hz","duty_percent","durations_us"],"additionalProperties":false},"outputSchema":{"type":"object","properties":{"sent":{"type":"boolean"},"duration_count":{"type":"integer"},"repeats":{"type":"integer"}},"required":["sent","duration_count","repeats"]},"annotations":{"readOnlyHint":false,"destructiveHint":true,"idempotentHint":false,"openWorldHint":false}},
{"name":"download","title":"Download file from STB","description":"At a Linux shell prompt, verify and cache one STB file up to 2 MiB. Returns hashes, transport metadata and an HTTP download URL; the bytes are not embedded in MCP.","inputSchema":{"type":"object","properties":{"path":{"type":"string","minLength":1,"maxLength":240},"timeout":{"type":"integer","minimum":1,"maximum":600,"default":600}},"required":["path"],"additionalProperties":false},"outputSchema":{"type":"object","properties":{"state":{"const":"complete"},"name":{"type":"string"},"size":{"type":"integer"},"sha256_source":{"type":"string"},"sha256_local":{"type":"string"},"transport":{"enum":["serial","nc"]},"seconds":{"type":"number"},"retries":{"type":"integer"},"board_ips":{"type":["array","null"]},"download_url":{"type":"string"}},"required":["state","name","size","sha256_source","sha256_local","transport","seconds","retries","board_ips","download_url"]},"annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}}
])tools";

esp_err_t receive_body(httpd_req_t* request, char* output,
                       const std::size_t length) {
    std::size_t received = 0;
    unsigned timeouts = 0;
    while (received < length) {
        const auto result =
            httpd_req_recv(request, output + received, length - received);
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

esp_err_t send_json(httpd_req_t* request, cJSON* response,
                    const char* protocol_version = nullptr) {
    if (response == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    char* body = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (body == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (protocol_version != nullptr) {
        httpd_resp_set_hdr(request, "MCP-Protocol-Version", protocol_version);
    }
    const auto result = httpd_resp_sendstr(request, body);
    cJSON_free(body);
    return result;
}

cJSON* make_rpc_response(const cJSON* id, cJSON* result) {
    auto* response = cJSON_CreateObject();
    if (response == nullptr || result == nullptr) {
        cJSON_Delete(response);
        cJSON_Delete(result);
        return nullptr;
    }
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id",
                          id == nullptr ? cJSON_CreateNull()
                                        : cJSON_Duplicate(id, true));
    cJSON_AddItemToObject(response, "result", result);
    return response;
}

cJSON* make_rpc_error(const cJSON* id, const int code, const char* message) {
    auto* response = cJSON_CreateObject();
    auto* error = cJSON_CreateObject();
    if (response == nullptr || error == nullptr) {
        cJSON_Delete(response);
        cJSON_Delete(error);
        return nullptr;
    }
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id",
                          id == nullptr ? cJSON_CreateNull()
                                        : cJSON_Duplicate(id, true));
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(response, "error", error);
    return response;
}

cJSON* tool_error(const char* message) {
    auto* result = cJSON_CreateObject();
    if (result != nullptr) {
        cJSON_AddStringToObject(result, "error", message);
    }
    return result;
}

bool integer_value(const cJSON* object, const char* name,
                   const std::uint64_t minimum, const std::uint64_t maximum,
                   std::uint64_t* output, const bool required,
                   const std::uint64_t default_value = 0) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == nullptr) {
        if (required) {
            return false;
        }
        *output = default_value;
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
        std::floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < static_cast<double>(minimum) ||
        item->valuedouble > static_cast<double>(maximum)) {
        return false;
    }
    *output = static_cast<std::uint64_t>(item->valuedouble);
    return true;
}

bool boolean_value(const cJSON* object, const char* name, bool* output,
                   const bool default_value) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == nullptr) {
        *output = default_value;
        return true;
    }
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *output = cJSON_IsTrue(item);
    return true;
}

const char* string_value(const cJSON* object, const char* name,
                         const std::size_t minimum,
                         const std::size_t maximum, const bool required,
                         bool* valid) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (item == nullptr) {
        *valid = !required;
        return nullptr;
    }
    if (!cJSON_IsString(item)) {
        *valid = false;
        return nullptr;
    }
    const auto length = std::strlen(item->valuestring);
    *valid = length >= minimum && length <= maximum;
    return *valid ? item->valuestring : nullptr;
}

std::size_t base64_size(const std::size_t length) {
    return ((length + 2) / 3) * 4;
}

char* base64_encode(const std::uint8_t* input, const std::size_t length) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto output_length = base64_size(length);
    auto* output = static_cast<char*>(heap_caps_malloc(
        output_length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (output == nullptr) {
        return nullptr;
    }
    std::size_t source = 0;
    std::size_t target = 0;
    while (source < length) {
        const auto remaining = length - source;
        const std::uint32_t a = input[source++];
        const std::uint32_t b = remaining > 1 ? input[source++] : 0;
        const std::uint32_t c = remaining > 2 ? input[source++] : 0;
        const auto value = (a << 16) | (b << 8) | c;
        output[target++] = alphabet[(value >> 18) & 0x3f];
        output[target++] = alphabet[(value >> 12) & 0x3f];
        output[target++] = remaining > 1 ? alphabet[(value >> 6) & 0x3f] : '=';
        output[target++] = remaining > 2 ? alphabet[value & 0x3f] : '=';
    }
    output[target] = '\0';
    return output;
}

bool continuation(const std::uint8_t value) {
    return (value & 0xc0) == 0x80;
}

char* utf8_safe(const std::uint8_t* input, const std::size_t length) {
    auto* output = static_cast<char*>(heap_caps_malloc(
        length * 3 + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (output == nullptr) {
        return nullptr;
    }
    std::size_t source = 0;
    std::size_t target = 0;
    while (source < length) {
        const auto first = input[source];
        std::size_t count = 0;
        if (first >= 0x20 && first <= 0x7f) {
            count = 1;
        } else if (first == '\n' || first == '\r' || first == '\t' ||
                   (first > 0 && first < 0x20)) {
            count = 1;
        } else if (first >= 0xc2 && first <= 0xdf && source + 1 < length &&
                   continuation(input[source + 1])) {
            count = 2;
        } else if (first >= 0xe0 && first <= 0xef && source + 2 < length &&
                   continuation(input[source + 1]) &&
                   continuation(input[source + 2]) &&
                   !(first == 0xe0 && input[source + 1] < 0xa0) &&
                   !(first == 0xed && input[source + 1] >= 0xa0)) {
            count = 3;
        } else if (first >= 0xf0 && first <= 0xf4 && source + 3 < length &&
                   continuation(input[source + 1]) &&
                   continuation(input[source + 2]) &&
                   continuation(input[source + 3]) &&
                   !(first == 0xf0 && input[source + 1] < 0x90) &&
                   !(first == 0xf4 && input[source + 1] >= 0x90)) {
            count = 4;
        }
        if (count == 0) {
            output[target++] = static_cast<char>(0xef);
            output[target++] = static_cast<char>(0xbf);
            output[target++] = static_cast<char>(0xbd);
            ++source;
        } else {
            std::memcpy(output + target, input + source, count);
            target += count;
            source += count;
        }
    }
    output[target] = '\0';
    return output;
}

cJSON* bytes_result(const std::uint8_t* bytes, const std::size_t length,
                    const std::uint64_t start) {
    auto* text = utf8_safe(bytes, length);
    auto* base64 = base64_encode(bytes, length);
    auto* result = cJSON_CreateObject();
    if (text == nullptr || base64 == nullptr || result == nullptr) {
        heap_caps_free(text);
        heap_caps_free(base64);
        cJSON_Delete(result);
        return nullptr;
    }
    cJSON_AddStringToObject(result, "text", text);
    cJSON_AddStringToObject(result, "data_base64", base64);
    cJSON_AddNumberToObject(result, "start", static_cast<double>(start));
    cJSON_AddNumberToObject(result, "next",
                            static_cast<double>(start + length));
    heap_caps_free(text);
    heap_caps_free(base64);
    return result;
}

cJSON* uart_config_json(const UartRuntimeConfig& config) {
    auto* result = cJSON_CreateObject();
    if (result != nullptr) {
        cJSON_AddNumberToObject(result, "baud", config.baud);
        cJSON_AddNumberToObject(result, "data_bits", config.data_bits);
        cJSON_AddStringToObject(result, "parity",
                                uart_parity_name(config.parity));
        cJSON_AddNumberToObject(
            result, "stop_bits",
            static_cast<double>(config.stop_bits_x2) / 2.0);
    }
    return result;
}

}  // namespace

esp_err_t McpService::init(RingLog* history, UartConsole* uart,
                           IrBlaster* ir, WifiService* wifi,
                           RemoteStore* remotes,
                           FileDownloadService* downloads) {
    if (history == nullptr || uart == nullptr || ir == nullptr ||
        wifi == nullptr || remotes == nullptr || downloads == nullptr ||
        history_ != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    history_ = history;
    uart_ = uart;
    ir_ = ir;
    wifi_ = wifi;
    remotes_ = remotes;
    downloads_ = downloads;
    return ESP_OK;
}

esp_err_t McpService::handle(httpd_req_t* request) {
    const auto length = static_cast<std::size_t>(request->content_len);
    if (length == 0 || length > kMaximumRequestBytes) {
        httpd_resp_set_status(request, "413 Content Too Large");
        return httpd_resp_sendstr(request, "MCP body must be 1..24576 bytes");
    }
    auto* body = static_cast<char*>(heap_caps_malloc(
        length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (body == nullptr) {
        return send_json(request, make_rpc_error(nullptr, -32603,
                                                 "out of memory"));
    }
    if (receive_body(request, body, length) != ESP_OK) {
        heap_caps_free(body);
        return send_json(request,
                         make_rpc_error(nullptr, -32700, "incomplete JSON"));
    }
    body[length] = '\0';
    auto* message = cJSON_ParseWithLength(body, length);
    heap_caps_free(body);
    if (message == nullptr || !cJSON_IsObject(message)) {
        cJSON_Delete(message);
        return send_json(request,
                         make_rpc_error(nullptr, -32700, "parse error"));
    }

    const auto* jsonrpc = cJSON_GetObjectItemCaseSensitive(message, "jsonrpc");
    const auto* id = cJSON_GetObjectItemCaseSensitive(message, "id");
    const auto* method_json =
        cJSON_GetObjectItemCaseSensitive(message, "method");
    const auto* params = cJSON_GetObjectItemCaseSensitive(message, "params");
    if (!cJSON_IsString(jsonrpc) ||
        std::strcmp(jsonrpc->valuestring, "2.0") != 0 ||
        !cJSON_IsString(method_json) ||
        (id != nullptr && !cJSON_IsString(id) && !cJSON_IsNumber(id) &&
         !cJSON_IsNull(id))) {
        auto* response = make_rpc_error(id, -32600, "invalid request");
        cJSON_Delete(message);
        return send_json(request, response);
    }

    char version_header[32]{};
    const bool have_header =
        httpd_req_get_hdr_value_str(request, "MCP-Protocol-Version",
                                    version_header,
                                    sizeof(version_header)) == ESP_OK;
    const cJSON* meta = cJSON_IsObject(params)
                            ? cJSON_GetObjectItemCaseSensitive(params, "_meta")
                            : nullptr;
    const cJSON* meta_version =
        cJSON_IsObject(meta)
            ? cJSON_GetObjectItemCaseSensitive(
                  meta, "io.modelcontextprotocol/protocolVersion")
            : nullptr;
    const bool have_meta_version = cJSON_IsString(meta_version);
    const bool modern = (have_header &&
                         std::strcmp(version_header, kModernVersion) == 0) ||
                        (have_meta_version &&
                         std::strcmp(meta_version->valuestring,
                                     kModernVersion) == 0);
    if (modern && (!have_header || !have_meta_version ||
                   std::strcmp(version_header, meta_version->valuestring) !=
                       0)) {
        auto* response = make_rpc_error(
            id, -32602, "MCP protocol header and request envelope differ");
        cJSON_Delete(message);
        return send_json(request, response, kModernVersion);
    }

    const char* method = method_json->valuestring;
    if (id == nullptr) {
        ESP_LOGD(kTag, "accepted notification %s", method);
        cJSON_Delete(message);
        httpd_resp_set_status(request, "202 Accepted");
        return httpd_resp_send(request, nullptr, 0);
    }

    cJSON* response = nullptr;
    const char* response_version = modern ? kModernVersion : nullptr;
    if (std::strcmp(method, "server/discover") == 0) {
        auto* result = cJSON_CreateObject();
        auto* versions = cJSON_AddArrayToObject(result, "supportedVersions");
        cJSON_AddItemToArray(versions, cJSON_CreateString(kModernVersion));
        cJSON_AddItemToArray(versions, cJSON_CreateString(kLegacyVersion));
        auto* capabilities = cJSON_AddObjectToObject(result, "capabilities");
        auto* tools = cJSON_AddObjectToObject(capabilities, "tools");
        cJSON_AddBoolToObject(tools, "listChanged", false);
        cJSON_AddStringToObject(result, "instructions", kInstructions);
        cJSON_AddNumberToObject(result, "ttlMs", 60000);
        cJSON_AddStringToObject(result, "cacheScope", "public");
        cJSON_AddStringToObject(result, "resultType", "complete");
        auto* result_meta = cJSON_AddObjectToObject(result, "_meta");
        auto* server_info = cJSON_AddObjectToObject(
            result_meta, "io.modelcontextprotocol/serverInfo");
        cJSON_AddStringToObject(server_info, "name", "stb-buddy");
        cJSON_AddStringToObject(server_info, "version",
                                esp_app_get_description()->version);
        response = make_rpc_response(id, result);
        response_version = kModernVersion;
    } else if (std::strcmp(method, "initialize") == 0) {
        const auto* requested = cJSON_IsObject(params)
                                    ? cJSON_GetObjectItemCaseSensitive(
                                          params, "protocolVersion")
                                    : nullptr;
        const char* negotiated = kLegacyVersion;
        if (cJSON_IsString(requested)) {
            if (std::strcmp(requested->valuestring, "2025-06-18") == 0) {
                negotiated = "2025-06-18";
            } else if (std::strcmp(requested->valuestring, "2025-03-26") ==
                       0) {
                negotiated = "2025-03-26";
            } else if (std::strcmp(requested->valuestring, "2024-11-05") ==
                       0) {
                negotiated = "2024-11-05";
            }
        }
        auto* result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "protocolVersion", negotiated);
        auto* capabilities = cJSON_AddObjectToObject(result, "capabilities");
        auto* tools = cJSON_AddObjectToObject(capabilities, "tools");
        cJSON_AddBoolToObject(tools, "listChanged", false);
        auto* server_info = cJSON_AddObjectToObject(result, "serverInfo");
        cJSON_AddStringToObject(server_info, "name", "stb-buddy");
        cJSON_AddStringToObject(server_info, "title", "STB Buddy");
        cJSON_AddStringToObject(server_info, "version",
                                esp_app_get_description()->version);
        cJSON_AddStringToObject(result, "instructions", kInstructions);
        response = make_rpc_response(id, result);
        response_version = negotiated;
    } else if (std::strcmp(method, "ping") == 0) {
        response = make_rpc_response(id, cJSON_CreateObject());
    } else if (std::strcmp(method, "tools/list") == 0) {
        auto* result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "tools", cJSON_Parse(kToolsJson));
        cJSON_AddNumberToObject(result, "ttlMs", 60000);
        cJSON_AddStringToObject(result, "cacheScope", "public");
        cJSON_AddStringToObject(result, "resultType", "complete");
        response = make_rpc_response(id, result);
    } else if (std::strcmp(method, "tools/call") == 0) {
        const auto* name = cJSON_IsObject(params)
                               ? cJSON_GetObjectItemCaseSensitive(params, "name")
                               : nullptr;
        const auto* arguments =
            cJSON_IsObject(params)
                ? cJSON_GetObjectItemCaseSensitive(params, "arguments")
                : nullptr;
        if (!cJSON_IsString(name) ||
            (arguments != nullptr && !cJSON_IsObject(arguments))) {
            response = make_rpc_error(id, -32602,
                                      "tool name and object arguments required");
        } else {
            bool is_error = false;
            auto* empty_arguments =
                arguments == nullptr ? cJSON_CreateObject() : nullptr;
            auto* structured = dispatch_tool(
                name->valuestring,
                arguments == nullptr ? empty_arguments : arguments,
                &is_error);
            cJSON_Delete(empty_arguments);
            if (structured == nullptr) {
                response = make_rpc_error(id, -32603, "out of memory");
            } else {
                char* text = cJSON_PrintUnformatted(structured);
                auto* result = cJSON_CreateObject();
                auto* content = cJSON_AddArrayToObject(result, "content");
                auto* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "type", "text");
                cJSON_AddStringToObject(item, "text",
                                        text == nullptr ? "{}" : text);
                cJSON_AddItemToArray(content, item);
                cJSON_AddItemToObject(result, "structuredContent", structured);
                cJSON_AddBoolToObject(result, "isError", is_error);
                cJSON_AddStringToObject(result, "resultType", "complete");
                cJSON_free(text);
                response = make_rpc_response(id, result);
            }
        }
    } else {
        response = make_rpc_error(id, -32601, "method not found");
    }

    cJSON_Delete(message);
    return send_json(request, response, response_version);
}

cJSON* McpService::dispatch_tool(const char* name, const cJSON* arguments,
                                 bool* is_error) {
    if (std::strcmp(name, "status") == 0) {
        return tool_status();
    }
    if (std::strcmp(name, "read") == 0) {
        return tool_read(arguments, is_error);
    }
    if (std::strcmp(name, "write") == 0) {
        return tool_write(arguments, is_error);
    }
    if (std::strcmp(name, "wait_for") == 0) {
        return tool_wait_for(arguments, is_error);
    }
    if (std::strcmp(name, "send_and_wait") == 0) {
        return tool_send_and_wait(arguments, is_error);
    }
    if (std::strcmp(name, "clear_history") == 0) {
        return tool_clear_history();
    }
    if (std::strcmp(name, "uart_configure") == 0) {
        return tool_uart_configure(arguments, is_error);
    }
    if (std::strcmp(name, "remotes_load") == 0) {
        return tool_remotes_load(arguments, is_error);
    }
    if (std::strcmp(name, "remotes_list") == 0) {
        return tool_remotes_list();
    }
    if (std::strcmp(name, "ir_send_key") == 0) {
        return tool_ir_send_key(arguments, is_error);
    }
    if (std::strcmp(name, "ir_send_nec") == 0) {
        return tool_ir_send_nec(arguments, is_error);
    }
    if (std::strcmp(name, "ir_send_raw") == 0) {
        return tool_ir_send_raw(arguments, is_error);
    }
    if (std::strcmp(name, "download") == 0) {
        return tool_download(arguments, is_error);
    }
    *is_error = true;
    return tool_error("unknown tool");
}

cJSON* McpService::tool_status() {
    const auto wifi = wifi_->snapshot();
    const auto history = history_->snapshot();
    const auto uart = uart_->configuration();
    auto* result = cJSON_CreateObject();
    auto* wifi_json = cJSON_AddObjectToObject(result, "wifi");
    auto* uart_json = uart_config_json(uart);
    cJSON_AddItemToObject(result, "uart", uart_json);
    auto* history_json = cJSON_AddObjectToObject(result, "history");
    auto* memory_json = cJSON_AddObjectToObject(result, "memory");
    auto* ir_json = cJSON_AddObjectToObject(result, "ir");
    cJSON_AddItemToObject(result, "download",
                          download_snapshot_json(downloads_->snapshot()));
    cJSON_AddStringToObject(result, "name", "stb-buddy");
    cJSON_AddStringToObject(result, "version",
                            esp_app_get_description()->version);
    cJSON_AddStringToObject(wifi_json, "mode",
                            wifi.setup_mode ? "setup-ap" : "station");
    cJSON_AddBoolToObject(wifi_json, "connected", wifi.connected);
    cJSON_AddStringToObject(wifi_json, "ssid", wifi.ssid.data());
    cJSON_AddStringToObject(wifi_json, "ip", wifi.ip.data());
    cJSON_AddBoolToObject(uart_json, "connected", uart_->connected());
    cJSON_AddNumberToObject(uart_json, "rx_gpio", 1);
    cJSON_AddNumberToObject(uart_json, "tx_gpio", 2);
    cJSON_AddNumberToObject(history_json, "start",
                            static_cast<double>(history.start));
    cJSON_AddNumberToObject(history_json, "end",
                            static_cast<double>(history.end));
    cJSON_AddNumberToObject(history_json, "bytes", history.size);
    cJSON_AddNumberToObject(history_json, "capacity", history.capacity);
    cJSON_AddNumberToObject(
        memory_json, "free_internal",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(memory_json, "free_psram",
                            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddBoolToObject(ir_json, "ready", ir_->ready());
    cJSON_AddNumberToObject(ir_json, "gpio", 47);
    cJSON_AddNumberToObject(ir_json, "remote_count",
                            remotes_->remote_count());
    cJSON_AddNumberToObject(ir_json, "key_count", remotes_->key_count());
    return result;
}

cJSON* McpService::tool_read(const cJSON* arguments, bool* is_error) {
    std::uint64_t maximum = 32768;
    if (!integer_value(arguments, "max_bytes", 1, kMaximumReadBytes,
                       &maximum, false, 32768)) {
        *is_error = true;
        return tool_error("max_bytes must be 1..65536");
    }
    std::uint64_t since = 0;
    const auto* since_json =
        cJSON_GetObjectItemCaseSensitive(arguments, "since");
    if (since_json == nullptr) {
        const auto snapshot = history_->snapshot();
        since = snapshot.end -
                std::min<std::uint64_t>(snapshot.size, maximum);
    } else if (!integer_value(arguments, "since", 0, UINT64_MAX, &since,
                              true)) {
        *is_error = true;
        return tool_error("since must be a non-negative integer");
    }
    auto* buffer = static_cast<std::uint8_t*>(heap_caps_malloc(
        static_cast<std::size_t>(maximum),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        return nullptr;
    }
    std::uint64_t actual_start = 0;
    const auto count = history_->read(since, buffer,
                                      static_cast<std::size_t>(maximum),
                                      &actual_start);
    auto* result = bytes_result(buffer, count, actual_start);
    heap_caps_free(buffer);
    return result;
}

cJSON* McpService::tool_write(const cJSON* arguments, bool* is_error) {
    bool valid = false;
    const char* data = string_value(arguments, "data", 0,
                                    kMaximumWriteBytes, true, &valid);
    bool newline = true;
    const auto data_length = valid ? std::strlen(data) : 0;
    if (!valid || !boolean_value(arguments, "newline", &newline, true) ||
        (data_length + (newline ? 1U : 0U)) > kMaximumWriteBytes) {
        *is_error = true;
        return tool_error("data plus optional Enter must be at most 4096 bytes");
    }
    const auto offset = history_->snapshot().end;
    std::size_t written = 0;
    if (uart_->write(reinterpret_cast<const std::uint8_t*>(data),
                     data_length, &written) != ESP_OK) {
        *is_error = true;
        return tool_error("UART write failed");
    }
    if (newline) {
        const std::uint8_t carriage_return = '\r';
        std::size_t newline_written = 0;
        if (uart_->write(&carriage_return, 1, &newline_written) != ESP_OK) {
            *is_error = true;
            return tool_error("UART Enter write failed");
        }
        written += newline_written;
    }
    uart_->record_note("MCP", reinterpret_cast<const std::uint8_t*>(data),
                       data_length, newline);
    auto* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "written", written);
    cJSON_AddNumberToObject(result, "offset", static_cast<double>(offset));
    return result;
}

cJSON* McpService::wait_for_pattern(const char* pattern,
                                    std::uint64_t since,
                                    const std::uint32_t timeout_ms,
                                    const char* reply, bool* is_error) {
    regex_t expression{};
    const auto compile_result =
        regcomp(&expression, pattern, REG_EXTENDED | REG_NEWLINE);
    if (compile_result != 0) {
        std::array<char, 128> message{};
        regerror(compile_result, &expression, message.data(), message.size());
        *is_error = true;
        return tool_error(message.data());
    }

    auto* buffer = static_cast<std::uint8_t*>(heap_caps_malloc(
        kMaximumReadBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        regfree(&expression);
        return nullptr;
    }
    const auto deadline = esp_timer_get_time() +
                          static_cast<std::int64_t>(timeout_ms) * 1000;
    bool matched = false;
    std::array<char, 257> match_text{};
    std::uint64_t actual_start = since;
    std::size_t count = 0;
    do {
        const auto snapshot = history_->snapshot();
        if (snapshot.end > since + kMaximumReadBytes) {
            since = snapshot.end - kMaximumReadBytes;
        }
        count = history_->read(since, buffer, kMaximumReadBytes, &actual_start);
        auto* text = utf8_safe(buffer, count);
        if (text == nullptr) {
            heap_caps_free(buffer);
            regfree(&expression);
            return nullptr;
        }
        regmatch_t match{};
        matched = regexec(&expression, text, 1, &match, 0) == 0;
        if (matched && match.rm_so >= 0 && match.rm_eo >= match.rm_so) {
            const auto length = std::min<std::size_t>(
                static_cast<std::size_t>(match.rm_eo - match.rm_so),
                match_text.size() - 1);
            std::memcpy(match_text.data(), text + match.rm_so, length);
            match_text[length] = '\0';
        }
        heap_caps_free(text);
        if (matched || esp_timer_get_time() >= deadline) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    } while (true);
    regfree(&expression);

    if (matched && reply != nullptr) {
        std::size_t written = 0;
        const auto reply_length = std::strlen(reply);
        if (uart_->write(reinterpret_cast<const std::uint8_t*>(reply),
                         reply_length, &written) != ESP_OK ||
            written != reply_length) {
            heap_caps_free(buffer);
            *is_error = true;
            return tool_error("matched, but immediate UART reply failed");
        }
        uart_->record_note(
            "MCP reply", reinterpret_cast<const std::uint8_t*>(reply),
            reply_length);
    }

    auto* result = bytes_result(buffer, count, actual_start);
    heap_caps_free(buffer);
    if (result != nullptr) {
        cJSON_AddBoolToObject(result, "matched", matched);
        if (matched) {
            cJSON_AddStringToObject(result, "match", match_text.data());
        } else {
            cJSON_AddNullToObject(result, "match");
        }
    }
    return result;
}

cJSON* McpService::tool_wait_for(const cJSON* arguments, bool* is_error) {
    bool valid = false;
    const char* pattern = string_value(arguments, "pattern", 1, 256, true,
                                       &valid);
    if (!valid) {
        *is_error = true;
        return tool_error("pattern must be 1..256 bytes");
    }
    std::uint64_t timeout = 10000;
    if (!integer_value(arguments, "timeout_ms", 0, kMaximumWaitMs, &timeout,
                       false, 10000)) {
        *is_error = true;
        return tool_error("timeout_ms must be 0..60000");
    }
    std::uint64_t since = 0;
    const auto* since_json =
        cJSON_GetObjectItemCaseSensitive(arguments, "since");
    if (since_json == nullptr) {
        since = history_->snapshot().end;
    } else if (!integer_value(arguments, "since", 0, UINT64_MAX, &since,
                              true)) {
        *is_error = true;
        return tool_error("since must be a non-negative integer");
    }
    bool reply_valid = false;
    const char* reply = string_value(arguments, "reply", 0,
                                     kMaximumWriteBytes, false,
                                     &reply_valid);
    if (!reply_valid) {
        *is_error = true;
        return tool_error("reply must be at most 4096 bytes");
    }
    if (reply != nullptr) {
        uart_->begin_transaction();
    }
    auto* result = wait_for_pattern(pattern, since,
                                    static_cast<std::uint32_t>(timeout), reply,
                                    is_error);
    if (reply != nullptr) {
        uart_->end_transaction();
    }
    return result;
}

cJSON* McpService::tool_send_and_wait(const cJSON* arguments,
                                      bool* is_error) {
    bool valid = false;
    const char* command = string_value(arguments, "command", 1,
                                       kMaximumWriteBytes - 1, true, &valid);
    if (!valid) {
        *is_error = true;
        return tool_error("command must be 1..4095 bytes");
    }
    bool pattern_valid = false;
    const char* pattern = string_value(arguments, "pattern", 1, 256, false,
                                       &pattern_valid);
    if (!pattern_valid) {
        *is_error = true;
        return tool_error("pattern must be 1..256 bytes");
    }
    if (pattern == nullptr) {
        pattern = "# ";
    }
    std::uint64_t timeout = 10000;
    if (!integer_value(arguments, "timeout_ms", 0, kMaximumWaitMs, &timeout,
                       false, 10000)) {
        *is_error = true;
        return tool_error("timeout_ms must be 0..60000");
    }
    bool reply_valid = false;
    const char* reply = string_value(arguments, "reply", 0,
                                     kMaximumWriteBytes, false,
                                     &reply_valid);
    if (!reply_valid) {
        *is_error = true;
        return tool_error("reply must be at most 4096 bytes");
    }

    uart_->begin_transaction();
    const auto since = history_->snapshot().end;
    std::size_t written = 0;
    auto result = uart_->write(reinterpret_cast<const std::uint8_t*>(command),
                               std::strlen(command), &written);
    const std::uint8_t carriage_return = '\r';
    std::size_t newline_written = 0;
    if (result == ESP_OK) {
        result = uart_->write(&carriage_return, 1, &newline_written);
    }
    cJSON* response = nullptr;
    if (result != ESP_OK) {
        *is_error = true;
        response = tool_error("UART command write failed");
    } else {
        uart_->record_note(
            "MCP", reinterpret_cast<const std::uint8_t*>(command),
            std::strlen(command), true);
        response = wait_for_pattern(pattern, since,
                                    static_cast<std::uint32_t>(timeout), reply,
                                    is_error);
    }
    uart_->end_transaction();
    return response;
}

cJSON* McpService::tool_clear_history() {
    const auto before = history_->snapshot();
    const auto next = history_->clear();
    const auto cleared_notes = uart_->clear_notes();
    auto* result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "cleared_bytes", before.size);
    cJSON_AddNumberToObject(result, "cleared_notes", cleared_notes);
    cJSON_AddNumberToObject(result, "next", static_cast<double>(next));
    return result;
}

cJSON* McpService::tool_uart_configure(const cJSON* arguments,
                                       bool* is_error) {
    std::uint64_t baud = 0;
    std::uint64_t data_bits = 0;
    bool parity_valid = false;
    const char* parity = string_value(arguments, "parity", 3, 4, true,
                                      &parity_valid);
    const auto* stop = cJSON_GetObjectItemCaseSensitive(arguments, "stop_bits");
    if (!integer_value(arguments, "baud", 1200, 3'000'000, &baud, true) ||
        !integer_value(arguments, "data_bits", 5, 8, &data_bits, true) ||
        !parity_valid || !cJSON_IsNumber(stop)) {
        *is_error = true;
        return tool_error("invalid UART configuration");
    }
    UartParity parity_value = UartParity::none;
    if (std::strcmp(parity, "even") == 0) {
        parity_value = UartParity::even;
    } else if (std::strcmp(parity, "odd") == 0) {
        parity_value = UartParity::odd;
    } else if (std::strcmp(parity, "none") != 0) {
        *is_error = true;
        return tool_error("parity must be none, even or odd");
    }
    std::uint8_t stop_x2 = 0;
    if (stop->valuedouble == 1.0) {
        stop_x2 = 2;
    } else if (stop->valuedouble == 1.5) {
        stop_x2 = 3;
    } else if (stop->valuedouble == 2.0) {
        stop_x2 = 4;
    } else {
        *is_error = true;
        return tool_error("stop_bits must be 1, 1.5 or 2");
    }
    const UartRuntimeConfig config{
        .baud = static_cast<std::uint32_t>(baud),
        .data_bits = static_cast<std::uint8_t>(data_bits),
        .parity = parity_value,
        .stop_bits_x2 = stop_x2,
    };
    if (uart_->configure(config) != ESP_OK) {
        *is_error = true;
        return tool_error("UART driver rejected the configuration");
    }
    return uart_config_json(config);
}

cJSON* McpService::tool_remotes_load(const cJSON* arguments,
                                     bool* is_error) {
    bool valid = false;
    const char* config = string_value(arguments, "config", 1,
                                      kLircdMaxConfigBytes, true, &valid);
    if (!valid) {
        *is_error = true;
        return tool_error("config must be a complete 1..8192-byte lircd.conf");
    }
    LircdParseError error{};
    const auto load_result =
        remotes_->load(std::string_view{config, std::strlen(config)}, &error);
    if (load_result != ESP_OK) {
        *is_error = true;
        auto* result = tool_error(load_result == ESP_ERR_INVALID_ARG
                                      ? error.message.data()
                                      : "could not persist lircd.conf");
        if (load_result == ESP_ERR_INVALID_ARG) {
            cJSON_AddNumberToObject(result, "line", error.line);
        }
        return result;
    }
    auto* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "loaded", true);
    cJSON_AddNumberToObject(result, "remote_count", remotes_->remote_count());
    cJSON_AddNumberToObject(result, "key_count", remotes_->key_count());
    return result;
}

cJSON* McpService::tool_remotes_list() {
    void* memory = heap_caps_malloc(sizeof(LircdDatabase),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        return nullptr;
    }
    auto* database = new (memory) LircdDatabase{};
    if (!remotes_->snapshot(database)) {
        database->~LircdDatabase();
        heap_caps_free(database);
        return tool_error("remote store unavailable");
    }
    auto* result = cJSON_CreateObject();
    auto* list = cJSON_AddArrayToObject(result, "remotes");
    for (std::size_t remote_index = 0;
         remote_index < database->remote_count; ++remote_index) {
        const auto& remote = database->remotes[remote_index];
        auto* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", remote_index);
        cJSON_AddStringToObject(item, "name", remote.name.data());
        cJSON_AddStringToObject(item, "protocol",
                                protocol_name(remote.protocol));
        auto* keys = cJSON_AddArrayToObject(item, "keys");
        for (std::size_t offset = 0; offset < remote.key_count; ++offset) {
            cJSON_AddItemToArray(
                keys, cJSON_CreateString(
                          database->keys[remote.first_key + offset].name.data()));
        }
        cJSON_AddItemToArray(list, item);
    }
    cJSON_AddNumberToObject(result, "remote_count", database->remote_count);
    cJSON_AddNumberToObject(result, "key_count", database->key_count);
    database->~LircdDatabase();
    heap_caps_free(database);
    return result;
}

cJSON* McpService::tool_ir_send_key(const cJSON* arguments, bool* is_error) {
    const auto* remote_json =
        cJSON_GetObjectItemCaseSensitive(arguments, "remote");
    bool key_valid = false;
    const char* key = string_value(arguments, "key", 1,
                                   kLircdNameBytes - 1, true, &key_valid);
    std::uint64_t repeats_value = 0;
    if (!key_valid ||
        !integer_value(arguments, "repeats", 0, 10, &repeats_value, false,
                       0)) {
        *is_error = true;
        return tool_error("key and repeats 0..10 are required");
    }
    bool by_index = false;
    std::size_t remote_index = 0;
    const char* remote_name = nullptr;
    if (cJSON_IsNumber(remote_json) && remote_json->valuedouble >= 0 &&
        remote_json->valuedouble < kLircdMaxRemotes &&
        std::floor(remote_json->valuedouble) == remote_json->valuedouble) {
        by_index = true;
        remote_index = static_cast<std::size_t>(remote_json->valuedouble);
    } else if (cJSON_IsString(remote_json) &&
               std::strlen(remote_json->valuestring) > 0 &&
               std::strlen(remote_json->valuestring) < kLircdNameBytes) {
        remote_name = remote_json->valuestring;
    } else {
        *is_error = true;
        return tool_error("remote must be an index or name");
    }
    LircdRemote remote{};
    std::uint64_t code = 0;
    if (remotes_->resolve_key(by_index, remote_index, remote_name, key, &remote,
                              &code) != ESP_OK) {
        *is_error = true;
        return tool_error("remote or key not found");
    }
    const auto repeats = static_cast<unsigned>(repeats_value);
    if (ir_->send(remote, code, repeats, key) != ESP_OK) {
        *is_error = true;
        return tool_error("IR transmission failed");
    }
    auto* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "sent", true);
    cJSON_AddStringToObject(result, "key", key);
    cJSON_AddNumberToObject(result, "repeats", repeats);
    return result;
}

cJSON* McpService::tool_ir_send_nec(const cJSON* arguments, bool* is_error) {
    std::uint64_t address = 0;
    std::uint64_t command = 0;
    std::uint64_t repeats = 0;
    bool extended = false;
    if (!integer_value(arguments, "address", 0, 65535, &address, true) ||
        !integer_value(arguments, "command", 0, 255, &command, true) ||
        !integer_value(arguments, "repeats", 0, 10, &repeats, false, 0) ||
        !boolean_value(arguments, "extended_address", &extended, false) ||
        (!extended && address > 255)) {
        *is_error = true;
        return tool_error(
            "standard NEC address is 0..255; extended NEC address is 0..65535");
    }
    if (ir_->send_nec(static_cast<std::uint16_t>(address),
                      static_cast<std::uint8_t>(command), extended,
                      static_cast<unsigned>(repeats)) != ESP_OK) {
        *is_error = true;
        return tool_error("NEC transmission failed");
    }
    auto* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "sent", true);
    cJSON_AddNumberToObject(result, "address", static_cast<double>(address));
    cJSON_AddNumberToObject(result, "command", static_cast<double>(command));
    cJSON_AddBoolToObject(result, "extended_address", extended);
    cJSON_AddNumberToObject(result, "repeats", static_cast<double>(repeats));
    return result;
}

cJSON* McpService::tool_ir_send_raw(const cJSON* arguments, bool* is_error) {
    std::uint64_t carrier = 0;
    std::uint64_t duty = 0;
    std::uint64_t gap = 0;
    std::uint64_t repeats = 0;
    const auto* durations =
        cJSON_GetObjectItemCaseSensitive(arguments, "durations_us");
    if (!integer_value(arguments, "carrier_hz", 20000, 60000, &carrier, true) ||
        !integer_value(arguments, "duty_percent", 1, 99, &duty, true) ||
        !integer_value(arguments, "gap_us", 0, 1'000'000, &gap, false, 0) ||
        !integer_value(arguments, "repeats", 0, 10, &repeats, false, 0) ||
        !cJSON_IsArray(durations)) {
        *is_error = true;
        return tool_error("invalid raw IR arguments");
    }
    const auto count = cJSON_GetArraySize(durations);
    if (count <= 0 || count > static_cast<int>(kIrMaxDurations)) {
        *is_error = true;
        return tool_error("durations_us must contain 1..256 values");
    }
    std::array<std::uint32_t, kIrMaxDurations> values{};
    std::uint32_t total = 0;
    for (int index = 0; index < count; ++index) {
        const auto* item = cJSON_GetArrayItem(durations, index);
        if (!cJSON_IsNumber(item) || item->valuedouble < 1 ||
            item->valuedouble > 32767 ||
            std::floor(item->valuedouble) != item->valuedouble ||
            total > 1'000'000 -
                        static_cast<std::uint32_t>(item->valuedouble)) {
            *is_error = true;
            return tool_error(
                "durations must be 1..32767 us and total at most 1000000 us");
        }
        values[static_cast<std::size_t>(index)] =
            static_cast<std::uint32_t>(item->valuedouble);
        total += values[static_cast<std::size_t>(index)];
    }
    if (ir_->send_raw(static_cast<std::uint32_t>(carrier),
                      static_cast<std::uint8_t>(duty), values.data(),
                      static_cast<std::size_t>(count),
                      static_cast<std::uint32_t>(gap),
                      static_cast<unsigned>(repeats)) != ESP_OK) {
        *is_error = true;
        return tool_error("raw IR transmission failed");
    }
    auto* result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "sent", true);
    cJSON_AddNumberToObject(result, "duration_count", count);
    cJSON_AddNumberToObject(result, "repeats", static_cast<double>(repeats));
    return result;
}

cJSON* McpService::tool_download(const cJSON* arguments, bool* is_error) {
    bool path_valid = false;
    const char* path = string_value(arguments, "path", 1,
                                    kMaximumDownloadPathBytes, true,
                                    &path_valid);
    std::uint64_t timeout = kMaximumDownloadSeconds;
    if (!path_valid ||
        !integer_value(arguments, "timeout", 1, kMaximumDownloadSeconds,
                       &timeout, false, kMaximumDownloadSeconds)) {
        *is_error = true;
        return tool_error("path must be 1..240 bytes and timeout 1..600 seconds");
    }
    std::uint32_t generation = 0;
    const auto start = downloads_->start(
        path, static_cast<std::uint32_t>(timeout), &generation);
    if (start == DownloadStartResult::busy) {
        *is_error = true;
        return tool_error("another file download is running");
    }
    if (start == DownloadStartResult::invalid) {
        *is_error = true;
        return tool_error("invalid STB path or timeout");
    }
    if (start != DownloadStartResult::started) {
        *is_error = true;
        return tool_error("could not start file download");
    }
    DownloadSnapshot result{};
    if (!downloads_->wait(generation, static_cast<std::uint32_t>(timeout),
                          &result)) {
        *is_error = true;
        return tool_error("file download did not finish before the MCP wait ended");
    }
    if (result.state != DownloadState::complete) {
        *is_error = true;
        return tool_error(result.error[0] == '\0' ? "file download failed"
                                                   : result.error.data());
    }
    return download_snapshot_json(result);
}

}  // namespace stb_buddy
