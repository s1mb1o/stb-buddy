#include "remote_store.hpp"

#include <cstring>
#include <new>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"

namespace stb_buddy {

namespace {

constexpr auto kTag = "remotes";
constexpr auto kNvsNamespace = "stb_buddy";
constexpr auto kNvsKey = "lircd_conf";

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

}  // namespace

LircdDatabase* RemoteStore::allocate_database() {
    void* memory = heap_caps_calloc(1, sizeof(LircdDatabase),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == nullptr) {
        return nullptr;
    }
    return new (memory) LircdDatabase{};
}

void RemoteStore::free_database(LircdDatabase* database) {
    if (database != nullptr) {
        database->~LircdDatabase();
        heap_caps_free(database);
    }
}

esp_err_t RemoteStore::init() {
    if (mutex_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    mutex_ = xSemaphoreCreateMutex();
    active_ = allocate_database();
    if (mutex_ == nullptr || active_ == nullptr) {
        free_database(active_);
        active_ = nullptr;
        if (mutex_ != nullptr) {
            vSemaphoreDelete(mutex_);
            mutex_ = nullptr;
        }
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle = 0;
    auto result = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(kTag, "no persisted lircd.conf");
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }

    std::size_t length = 0;
    result = nvs_get_blob(handle, kNvsKey, nullptr, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        ESP_LOGI(kTag, "no persisted lircd.conf");
        return ESP_OK;
    }
    if (result != ESP_OK) {
        nvs_close(handle);
        return result;
    }
    if (length == 0 || length > kLircdMaxConfigBytes) {
        nvs_close(handle);
        ESP_LOGE(kTag, "invalid persisted lircd.conf size: %u",
                 static_cast<unsigned>(length));
        ESP_LOGW(kTag, "starting with an empty remote database");
        return ESP_OK;
    }

    auto* text = static_cast<char*>(heap_caps_malloc(
        length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* parsed = allocate_database();
    if (text == nullptr || parsed == nullptr) {
        heap_caps_free(text);
        free_database(parsed);
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    result = nvs_get_blob(handle, kNvsKey, text, &length);
    nvs_close(handle);
    if (result != ESP_OK) {
        heap_caps_free(text);
        free_database(parsed);
        return result;
    }

    LircdParseError error{};
    const bool valid =
        LircdParser::parse(std::string_view{text, length}, parsed, &error);
    heap_caps_free(text);
    if (!valid) {
        ESP_LOGE(kTag, "persisted lircd.conf is invalid at line %u: %s",
                 static_cast<unsigned>(error.line), error.message.data());
        free_database(parsed);
        ESP_LOGW(kTag, "starting with an empty remote database");
        return ESP_OK;
    }

    free_database(active_);
    active_ = parsed;
    ESP_LOGI(kTag, "restored %u remotes and %u keys",
             static_cast<unsigned>(active_->remote_count),
             static_cast<unsigned>(active_->key_count));
    return ESP_OK;
}

esp_err_t RemoteStore::load(const std::string_view configuration,
                            LircdParseError* error) {
    if (mutex_ == nullptr || configuration.empty() ||
        configuration.size() > kLircdMaxConfigBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    auto* parsed = allocate_database();
    if (parsed == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (!LircdParser::parse(configuration, parsed, error)) {
        free_database(parsed);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    auto result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, kNvsKey, configuration.data(),
                              configuration.size());
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    if (result != ESP_OK) {
        free_database(parsed);
        return result;
    }

    LircdDatabase* previous = nullptr;
    {
        Lock lock(mutex_);
        previous = active_;
        active_ = parsed;
    }
    free_database(previous);
    ESP_LOGI(kTag, "loaded %u remotes and %u keys",
             static_cast<unsigned>(parsed->remote_count),
             static_cast<unsigned>(parsed->key_count));
    return ESP_OK;
}

esp_err_t RemoteStore::read_configuration(char* output, std::size_t* length) {
    if (mutex_ == nullptr || output == nullptr || length == nullptr ||
        *length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    Lock lock(mutex_);
    nvs_handle_t handle = 0;
    auto result = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        result = nvs_get_blob(handle, kNvsKey, output, length);
        nvs_close(handle);
    }
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    return result;
}

bool RemoteStore::snapshot(LircdDatabase* output) {
    if (mutex_ == nullptr || output == nullptr) {
        return false;
    }
    Lock lock(mutex_);
    *output = *active_;
    return true;
}

esp_err_t RemoteStore::resolve_key(const bool by_index,
                                   const std::size_t remote_index,
                                   const char* remote_name,
                                   const char* key_name,
                                   LircdRemote* remote,
                                   std::uint64_t* code) {
    if (mutex_ == nullptr || key_name == nullptr || remote == nullptr ||
        code == nullptr || (!by_index && remote_name == nullptr)) {
        return ESP_ERR_INVALID_ARG;
    }
    Lock lock(mutex_);
    std::size_t selected = active_->remote_count;
    if (by_index) {
        if (remote_index < active_->remote_count) {
            selected = remote_index;
        }
    } else {
        for (std::size_t index = 0; index < active_->remote_count; ++index) {
            if (strcasecmp(active_->remotes[index].name.data(), remote_name) ==
                0) {
                selected = index;
                break;
            }
        }
    }
    if (selected == active_->remote_count) {
        return ESP_ERR_NOT_FOUND;
    }

    auto& selected_remote = active_->remotes[selected];
    for (std::size_t offset = 0; offset < selected_remote.key_count; ++offset) {
        const auto& key = active_->keys[selected_remote.first_key + offset];
        if (strcasecmp(key.name.data(), key_name) == 0) {
            selected_remote.toggle_state ^= selected_remote.toggle_bit_mask;
            *remote = selected_remote;
            *code = key.code;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

std::size_t RemoteStore::remote_count() {
    if (mutex_ == nullptr) {
        return 0;
    }
    Lock lock(mutex_);
    return active_->remote_count;
}

std::size_t RemoteStore::key_count() {
    if (mutex_ == nullptr) {
        return 0;
    }
    Lock lock(mutex_);
    return active_->key_count;
}

}  // namespace stb_buddy
