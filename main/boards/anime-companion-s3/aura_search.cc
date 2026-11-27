#include "aura_search.h"
#include <algorithm>
#include <ctime>
#include <memory>
#include <mutex>
#include "aura_service.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "provider_request.h"
#include "web_search_format.h"

namespace aura {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
std::mutex mu;
unsigned generation = 0, calls = 0;
time_t last_success = 0, attempted = 0;
int http_code = 0;
bool busy = false;
std::string status = "Untested", last_error;
struct Config {
    std::string key;
    bool enabled = true;
};
Config read() {
    Config cfg;
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK)
        return cfg;
    char buf[129]{};
    size_t size = sizeof(buf);
    if (nvs_get_str(h, "tavily_api_key", buf, &size) == ESP_OK)
        cfg.key = buf;
    uint8_t on = 1;
    if (nvs_get_u8(h, "tavily_enabled", &on) == ESP_OK)
        cfg.enabled = on != 0;
    nvs_close(h);
    return cfg;
}
time_t stamp() {
    auto t = time(nullptr);
    return t > 1700000000 ? t : 0;
}
}  // namespace
bool SearchAvailable() {
    std::lock_guard lock(mu);
    auto c = read();
    return c.enabled && !c.key.empty();
}
const char* ConfigureSearch(const char* key, int enabled) {
    std::lock_guard lock(mu);
    if (key && (strlen(key) > 128 || !std::all_of(key, key + strlen(key), [](unsigned char c) {
                    return c > 32 && c < 127;
                })))
        return "Invalid Tavily key";
    auto old = read();
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK)
        return "Cannot open search settings";
    esp_err_t e = ESP_OK;
    if (key && old.key != key)
        e = *key ? nvs_set_str(h, "tavily_api_key", key) : nvs_erase_key(h, "tavily_api_key");
    if (e == ESP_ERR_NVS_NOT_FOUND)
        e = ESP_OK;
    if (e == ESP_OK && enabled >= 0)
        e = nvs_set_u8(h, "tavily_enabled", enabled);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK)
        return "Cannot save search settings";
    ++generation;
    status = "Untested";
    last_error.clear();
    http_code = 0;
    attempted = last_success = 0;
    return nullptr;
}
std::expected<std::string, std::string> Search(const std::string& query, bool tool) {
    std::unique_lock lock(mu);
    auto cfg = read();
    auto version = generation;
    if (tool)
        ++calls;
    const auto started = stamp();
    int code = 0;
    auto finish = [&](const char* error) {
        attempted = started;
        http_code = code;
        status = error ? error : "Operational";
        if (error)
            last_error = error;
        else
            last_success = stamp();
        ESP_LOGI("AuraServices", "%s Tavily at=%lld http=%d success=%d calls=%u",
                 tool ? "self.tavily.web_search" : "portal.test", (long long)started, code, !error,
                 calls);
    };
    const char* error = !cfg.enabled      ? "Disabled"
                        : cfg.key.empty() ? "API key required"
                        : !IsOnline()     ? "Home Wi-Fi unavailable"
                        : query.empty() || query.size() > 400 ||
                                query.find_first_not_of(" \t\r\n") == std::string::npos
                            ? "Invalid search query"
                            : nullptr;
    if (error) {
        finish(error);
        return std::unexpected(error);
    }
    lock.unlock();
    Json j(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(j.get(), "query", query.c_str());
    cJSON_AddNumberToObject(j.get(), "max_results", 3);
    cJSON_AddBoolToObject(j.get(), "include_answer", true);
    std::unique_ptr<char, decltype(&cJSON_free)> payload(cJSON_PrintUnformatted(j.get()),
                                                         cJSON_free);
    std::string raw, result;
    if (!payload ||
        !ProviderRequest("https://api.tavily.com/search", raw, code, cfg.key, payload.get())) {
        error = code == 401 || code == 403   ? "Invalid Tavily key"
                : code == 429                ? "Tavily rate limit"
                : code == 432 || code == 433 ? "Tavily usage limit"
                : code <= 0                  ? "Internet or TLS connection unavailable"
                                             : "Tavily request failed";
    } else {
        Json response(cJSON_Parse(raw.c_str()), cJSON_Delete);
        if (!response || !cJSON_IsArray(cJSON_GetObjectItem(response.get(), "results")))
            error = "Invalid Tavily response";
        else {
            auto formatted = FormatWebSearchResults(raw, query);
            if (!formatted || formatted->find(cfg.key) != std::string::npos)
                error = "Unusable Tavily response";
            else
                result = std::move(*formatted);
        }
    }
    lock.lock();
    if (version != generation)
        return std::unexpected("Search settings changed");
    finish(error);
    if (error)
        return std::unexpected(error);
    return result;
}
const char* SearchCommand(const cJSON* j, bool home) {
    auto field = [&](const char* k) { return cJSON_GetObjectItemCaseSensitive(j, k); };
    auto action = cJSON_GetStringValue(field("action"));
    if (!strcmp(action, "tavily")) {
        auto key = field("key"), on = field("enabled");
        if (!cJSON_IsBool(on) || (key && !cJSON_IsString(key)))
            return "Invalid search settings";
        return ConfigureSearch(key ? key->valuestring : nullptr, cJSON_IsTrue(on));
    }
    if (!home || !IsOnline())
        return "Test on home Wi-Fi";
    if (!SearchAvailable())
        return "Enable search and save a key first";
    auto q = cJSON_GetStringValue(field("query"));
    if (!q || strlen(q) > 400 || !*q)
        return "Enter a search query";
    std::lock_guard lock(mu);
    if (busy)
        return "Search test already running";
    auto query = std::make_unique<std::string>(q);
    busy = true;
    status = "Testing";
    auto* owned = query.release();
    if (xTaskCreate(
            [](void* arg) {
                {
                    std::unique_ptr<std::string> q(static_cast<std::string*>(arg));
                    Search(*q, false);
                    std::lock_guard lock(mu);
                    busy = false;
                }
                vTaskDelete(nullptr);
            },
            "aura-search", 8192, owned, 1, nullptr) != pdPASS) {
        delete owned;
        busy = false;
        status = "Worker unavailable";
        return "Worker unavailable";
    }
    return nullptr;
}
void SearchSnapshot(cJSON* s, bool diagnostics) {
    std::lock_guard lock(mu);
    auto cfg = read();
    auto j = cJSON_AddObjectToObject(s, "tavily");
    cJSON_AddBoolToObject(j, "enabled", cfg.enabled);
    cJSON_AddBoolToObject(j, "key_saved", !cfg.key.empty());
    cJSON_AddStringToObject(j, "status",
                            !cfg.enabled      ? "Disabled"
                            : cfg.key.empty() ? "API key required"
                                              : status.c_str());
    cJSON_AddNumberToObject(j, "last_success", last_success);
    if (diagnostics) {
        cJSON_AddNumberToObject(j, "attempt", attempted);
        cJSON_AddNumberToObject(j, "http", http_code);
        cJSON_AddNumberToObject(j, "calls", calls);
        cJSON_AddStringToObject(j, "error", last_error.c_str());
    }
}
}  // namespace aura
