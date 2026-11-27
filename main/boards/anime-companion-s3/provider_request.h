#pragma once
#include <cstring>
#include <string>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
namespace aura {
struct Body {
    size_t limit;
    std::string text;
    bool overflow = false;
};
inline bool ProviderRequest(const std::string& url, std::string& response, int& code,
                            const std::string& key = {}, const char* payload = nullptr) {
    Body body{payload ? size_t(16384) : size_t(12288), {}, false};
    esp_http_client_config_t c{};
    c.url = url.c_str();
    c.timeout_ms = 4500;
    c.buffer_size = 1024;
    c.buffer_size_tx = 512;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.disable_auto_redirect = true;
    c.user_data = &body;
    c.event_handler = [](esp_http_client_event_t* e) -> esp_err_t {
        auto& b = *static_cast<Body*>(e->user_data);
        if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
            if (b.text.size() + e->data_len > b.limit) {
                b.overflow = true;
                return ESP_FAIL;
            }
            if (!b.overflow)
                b.text.append(static_cast<char*>(e->data), e->data_len);
        }
        return ESP_OK;
    };
    auto client = esp_http_client_init(&c);
    esp_err_t err = client ? ESP_OK : ESP_FAIL;
    if (client && payload) {
        auto bearer = "Bearer " + key;
        if (esp_http_client_set_method(client, HTTP_METHOD_POST) != ESP_OK ||
            esp_http_client_set_header(client, "Authorization", bearer.c_str()) != ESP_OK ||
            esp_http_client_set_header(client, "Content-Type", "application/json") != ESP_OK ||
            esp_http_client_set_post_field(client, payload, strlen(payload)) != ESP_OK)
            err = ESP_FAIL;
    }
    if (err == ESP_OK)
        err = esp_http_client_perform(client);
    code = client ? esp_http_client_get_status_code(client) : 0;
    if (client)
        esp_http_client_cleanup(client);
    if (err != ESP_OK || body.overflow || code != 200)
        return false;
    response = std::move(body.text);
    return true;
}
}  // namespace aura
