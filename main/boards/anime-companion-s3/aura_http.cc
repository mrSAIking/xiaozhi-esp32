#include <atomic>
#include <cstring>
#include <mutex>
#include "aura_service.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "mdns.h"

extern const uint8_t aura_html[] asm("_binary_aura_portal_html_gz_start");
extern const uint8_t aura_html_end[] asm("_binary_aura_portal_html_gz_end");
extern const uint8_t aura_logo[] asm("_binary_aura_logo_webp_start");
extern const uint8_t aura_logo_end[] asm("_binary_aura_logo_webp_end");
namespace {
std::mutex server_mu;
httpd_handle_t lan = nullptr;
bool advertise = false;
std::atomic<bool> desired{false};
int home_marker;
bool Home(httpd_req_t* r) { return r->user_ctx == &home_marker; }
std::string Header(httpd_req_t* r, const char* key) {
    size_t n = httpd_req_get_hdr_value_len(r, key);
    if (!n || n > 160)
        return {};
    std::string v(n + 1, '\0');
    if (httpd_req_get_hdr_value_str(r, key, v.data(), v.size()) != ESP_OK)
        return {};
    v.resize(n);
    return v;
}
std::string Address(bool home) {
    esp_netif_ip_info_t ip{};
    auto net = esp_netif_get_handle_from_ifkey(home ? "WIFI_STA_DEF" : "WIFI_AP_DEF");
    char out[20]{};
    if (net && esp_netif_get_ip_info(net, &ip) == ESP_OK)
        esp_ip4addr_ntoa(&ip.ip, out, sizeof(out));
    return out;
}
esp_err_t Deny(httpd_req_t* r, const char* status, const char* msg) {
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "text/plain");
    return httpd_resp_sendstr(r, msg);
}
esp_err_t Page(httpd_req_t* r) {
    httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, (const char*)aura_html, aura_html_end - aura_html);
}
esp_err_t Logo(httpd_req_t* r) {
    httpd_resp_set_type(r, "image/webp");
    return httpd_resp_send(r, (const char*)aura_logo, aura_logo_end - aura_logo);
}
esp_err_t Api(httpd_req_t* r) {
    bool home = Home(r);
    auto host = Header(r, "Host");
    if (host.ends_with(":80"))
        host.resize(host.size() - 3);
    if (host != Address(home) && !(home && host == "project-aura.local"))
        return Deny(r, "403 Forbidden", "Open the device IP or project-aura.local");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    if (home) {
        auto auth = Header(r, "Authorization");
        if (!auth.starts_with("Bearer ") || !aura::Authorized(auth.substr(7)))
            return Deny(r, "401 Unauthorized",
                        "Enter the access code created during hotspot setup.");
    }
    if (r->method == HTTP_POST) {
        if (Header(r, "X-Aura-Request") != "1" || Header(r, "Content-Type") != "application/json")
            return Deny(r, "403 Forbidden", "Invalid request");
        if (!r->content_len || r->content_len > 1600)
            return Deny(r, "400 Bad Request", "Invalid request size");
        std::string body(r->content_len, '\0');
        size_t n = 0;
        while (n < body.size()) {
            int got = httpd_req_recv(r, body.data() + n, body.size() - n);
            if (got <= 0)
                return ESP_FAIL;
            n += got;
        }
        auto* j = cJSON_ParseWithLength(body.c_str(), body.size());
        if (!j)
            return Deny(r, "400 Bad Request", "Invalid JSON");
        auto error = aura::Command(j, home);
        cJSON_Delete(j);
        if (error)
            return Deny(r, "400 Bad Request", error);
    }
    auto* state = aura::Snapshot(home);
    char* raw = cJSON_PrintUnformatted(state);
    cJSON_Delete(state);
    if (!raw)
        return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r, "application/json");
    auto e = httpd_resp_sendstr(r, raw);
    cJSON_free(raw);
    return e;
}
esp_err_t Routes(httpd_handle_t s, bool home) {
    for (auto url : {"/", "/done.html", "/aura-logo.webp", "/api/aura"}) {
        httpd_uri_t u{};
        u.uri = url;
        u.method = HTTP_GET;
        u.user_ctx = home ? &home_marker : nullptr;
        u.handler = strcmp(url, "/api/aura") == 0         ? Api
                    : strcmp(url, "/aura-logo.webp") == 0 ? Logo
                                                          : Page;
        auto e = httpd_register_uri_handler(s, &u);
        if (e != ESP_OK)
            return e;
    }
    httpd_uri_t u{};
    u.uri = "/api/aura";
    u.method = HTTP_POST;
    u.handler = Api;
    u.user_ctx = home ? &home_marker : nullptr;
    return httpd_register_uri_handler(s, &u);
}
void Manage(void*) {
    for (;;) {
        bool on = desired.load();
        {
            std::lock_guard lock(server_mu);
            wifi_mode_t mode = WIFI_MODE_NULL;
            if (on && desired.load() && !lan && esp_wifi_get_mode(&mode) == ESP_OK &&
                mode == WIFI_MODE_STA) {
                httpd_config_t c = HTTPD_DEFAULT_CONFIG();
                c.stack_size = 7168;
                c.max_open_sockets = 3;
                c.lru_purge_enable = true;
                c.recv_wait_timeout = 4;
                c.send_wait_timeout = 4;
                if (httpd_start(&lan, &c) == ESP_OK) {
                    if (Routes(lan, true) != ESP_OK) {
                        httpd_stop(lan);
                        lan = nullptr;
                    } else if (mdns_init() == ESP_OK) {
                        advertise = true;
                        mdns_hostname_set("project-aura");
                        mdns_instance_name_set("Project Aura");
                        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
                    }
                }
            } else if (!on && !desired.load() && lan) {
                httpd_stop(lan);
                lan = nullptr;
                if (advertise) {
                    mdns_free();
                    advertise = false;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(750));
    }
}
}  // namespace
extern "C" bool aura_allow_portal_write(httpd_req_t* r) {
    const auto ip = Address(false), host = Header(r, "Host");
    return !ip.empty() && (host == ip || host == ip + ":80") && Header(r, "X-Aura-Request") == "1";
}
extern "C" void aura_register_ap(httpd_handle_t s) { ESP_ERROR_CHECK(Routes(s, false)); }
extern "C" esp_err_t aura_ap_socket(httpd_handle_t, int fd) {
    sockaddr_storage ss{};
    socklen_t n = sizeof(ss);
    if (getsockname(fd, (sockaddr*)&ss, &n) != 0)
        return ESP_FAIL;
    auto ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip{};
    if (!ap || esp_netif_get_ip_info(ap, &ip) != ESP_OK)
        return ESP_FAIL;
    if (ss.ss_family == AF_INET)
        return reinterpret_cast<sockaddr_in*>(&ss)->sin_addr.s_addr == ip.ip.addr ? ESP_OK
                                                                                  : ESP_FAIL;
#if CONFIG_LWIP_IPV6
    if (ss.ss_family == AF_INET6) {
        auto& a = reinterpret_cast<sockaddr_in6*>(&ss)->sin6_addr;
        return IN6_IS_ADDR_V4MAPPED(&a) && memcmp(&a.s6_addr[12], &ip.ip.addr, 4) == 0 ? ESP_OK
                                                                                       : ESP_FAIL;
    }
#endif
    return ESP_FAIL;
}
namespace aura {
void StartControl() { xTaskCreate(Manage, "aura-control", 4096, nullptr, 1, nullptr); }
void Network(bool connected) {
    desired = connected;
    Online(connected);
}
void StopHome() {
    desired = false;
    std::lock_guard lock(server_mu);
    if (lan) {
        httpd_stop(lan);
        lan = nullptr;
    }
    if (advertise) {
        mdns_free();
        advertise = false;
    }
}
}  // namespace aura
