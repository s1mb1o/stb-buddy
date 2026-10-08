#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lircd.hpp"

namespace stb_buddy {

class RemoteStore final {
public:
    RemoteStore() = default;
    RemoteStore(const RemoteStore&) = delete;
    RemoteStore& operator=(const RemoteStore&) = delete;

    esp_err_t init();
    esp_err_t load(std::string_view configuration, LircdParseError* error);
    esp_err_t read_configuration(char* output, std::size_t* length);
    bool snapshot(LircdDatabase* output);
    esp_err_t resolve_key(bool by_index, std::size_t remote_index,
                          const char* remote_name, const char* key_name,
                          LircdRemote* remote, std::uint64_t* code);
    std::size_t remote_count();
    std::size_t key_count();

private:
    static LircdDatabase* allocate_database();
    static void free_database(LircdDatabase* database);

    SemaphoreHandle_t mutex_{nullptr};
    LircdDatabase* active_{nullptr};
};

}  // namespace stb_buddy
