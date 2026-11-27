#include "aura_service.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"
using std::string;

namespace aura {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json parse(const string& s) { return Json(cJSON_Parse(s.c_str()), cJSON_Delete); }
string serialize(cJSON* j) {
    char* s = cJSON_PrintUnformatted(j);
    string out = s ? s : "";
    cJSON_free(s);
    return out;
}
const cJSON* field(const cJSON* j, const char* k) { return cJSON_GetObjectItemCaseSensitive(j, k); }
string str(const cJSON* j, const char* k) {
    auto* v = field(j, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
bool flag(const cJSON* j, const char* k) { return cJSON_IsTrue(field(j, k)); }
double num(const cJSON* j, const char* k) { return cJSON_GetNumberValue(field(j, k)); }
void add(cJSON* j, const char* k, const string& s) { cJSON_AddStringToObject(j, k, s.c_str()); }
void number(cJSON* j, const char* k, double v) { cJSON_AddNumberToObject(j, k, v); }
struct Zone {
    const char* name;
    const char* rule;
};
constexpr Zone zones[] = {{"Asia/Kolkata", "IST-5:30"},
                          {"UTC", "UTC0"},
                          {"Asia/Kathmandu", "NPT-5:45"},
                          {"Asia/Dubai", "GST-4"},
                          {"Asia/Tokyo", "JST-9"},
                          {"Asia/Singapore", "SGT-8"},
                          {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0/2"},
                          {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
                          {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
                          {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
                          {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
                          {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
                          {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"}};
const char* rule(const string& name) {
    for (auto z : zones)
        if (name == z.name)
            return z.rule;
    return nullptr;
}
struct Config {
    string key, city, country, units = "metric", zone = "Asia/Kolkata", verifier;
    double lat = NAN, lon = NAN;
    bool enabled = false, automatic = true, h24 = false;
};
Config cfg;
std::mutex mu, ntp_mu;
std::atomic<bool> online{false};
std::atomic<uint32_t> synchronized_at{0};
bool ntp_running = false;
std::atomic<bool> manual_complete{false};
int64_t next_fetch = 0, last_update = 0;
unsigned generation = 0;
string cached, choices = "[]", status = "Weather is off";
enum class Job { None, Search, Test, Fetch };
struct Request {
    Job type = Job::None;
    Config settings;
    unsigned version = 0;
};
Request pending;
bool working = false;
TaskHandle_t worker = nullptr;
int64_t now() { return esp_timer_get_time() / 1000000; }
bool coord(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) && std::abs(lat) <= 90 && std::abs(lon) <= 180;
}
bool clean(const string& s, size_t max) {
    return s.size() <= max &&
           std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool hex(const string& s) {
    return s.size() == 32 &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}
string digest(const string& token) {
    unsigned char hash[32];
    size_t n = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t*)token.data(), token.size(), hash,
                         sizeof(hash), &n) != PSA_SUCCESS ||
        n != 32)
        return {};
    constexpr char digits[] = "0123456789abcdef";
    string result;
    for (auto c : hash) {
        result += digits[c >> 4];
        result += digits[c & 15];
    }
    return result;
}
cJSON* settings(const Config& c) {
    auto* j = cJSON_CreateObject();
    add(j, "city", c.city);
    add(j, "country", c.country);
    add(j, "units", c.units);
    add(j, "zone", c.zone);
    cJSON_AddBoolToObject(j, "enabled", c.enabled);
    cJSON_AddBoolToObject(j, "automatic", c.automatic);
    cJSON_AddBoolToObject(j, "h24", c.h24);
    if (coord(c.lat, c.lon)) {
        number(j, "lat", c.lat);
        number(j, "lon", c.lon);
    }
    return j;
}
bool save(const Config& c) {
    Json j(settings(c), cJSON_Delete);
    add(j.get(), "key", c.key);
    add(j.get(), "verifier", c.verifier);
    auto raw = serialize(j.get());
    nvs_handle_t h;
    if (raw.empty() || nvs_open("aura_services", NVS_READWRITE, &h) != ESP_OK)
        return false;
    auto err = nvs_set_str(h, "config", raw.c_str());
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}
void load() {
    nvs_handle_t h;
    if (nvs_open("aura_services", NVS_READONLY, &h) != ESP_OK)
        return;
    size_t n = 0;
    if (nvs_get_str(h, "config", nullptr, &n) == ESP_OK && n && n <= 2048) {
        string raw(n, '\0');
        if (nvs_get_str(h, "config", raw.data(), &n) == ESP_OK) {
            auto j = parse(raw);
            if (j) {
                cfg.city = str(j.get(), "city");
                cfg.key = str(j.get(), "key");
                cfg.country = str(j.get(), "country");
                cfg.units = str(j.get(), "units");
                cfg.verifier = str(j.get(), "verifier");
                cfg.zone = str(j.get(), "zone");
                if (!rule(cfg.zone))
                    cfg.zone = "Asia/Kolkata";
                cfg.lat = num(j.get(), "lat");
                cfg.lon = num(j.get(), "lon");
                cfg.enabled = flag(j.get(), "enabled");
                cfg.automatic = flag(j.get(), "automatic");
                cfg.h24 = flag(j.get(), "h24");
            }
        }
    }
    nvs_close(h);
}
void sync(bool start, bool force = false) {
    std::lock_guard lock(ntp_mu);
    if (ntp_running && (!start || force)) {
        esp_netif_sntp_deinit();
        ntp_running = false;
    }
    if (start && !ntp_running) {
        esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
        c.num_of_servers = 2;
        c.servers[1] = "time.google.com";
#endif
        c.sync_cb = [](struct timeval* tv) {
            synchronized_at.store(tv->tv_sec);
            manual_complete = true;
        };
        ntp_running = esp_netif_sntp_init(&c) == ESP_OK;
    }
}
cJSON* clock_locked() {
    auto* j = cJSON_CreateObject();
    add(j, "zone", cfg.zone);
    auto stamp = synchronized_at.load();
    cJSON_AddBoolToObject(j, "synchronized", stamp > 0);
    number(j, "last_sync", stamp);
    if (!stamp)
        return j;
    time_t t = time(nullptr);
    number(j, "epoch", t);
    tm local{};
    localtime_r(&t, &local);
    char value[64];
    for (auto pair : {std::pair{"time", cfg.h24 ? "%H:%M:%S" : "%I:%M:%S %p"},
                      {"date", "%Y-%m-%d"},
                      {"weekday", "%A"},
                      {"offset", "%z"}}) {
        if (strftime(value, sizeof(value), pair.second, &local))
            add(j, pair.first, value);
    }
    return j;
}
string escape(const string& in) {
    constexpr char h[] = "0123456789ABCDEF";
    string out;
    for (unsigned char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += h[c >> 4];
            out += h[c & 15];
        }
    }
    return out;
}
struct Body {
    string text;
    bool overflow = false;
};
Json fetch(const string& url, string& error) {
    Body body;
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
            if (b.text.size() + e->data_len > 12288) {
                b.overflow = true;
                return ESP_FAIL;
            }
            if (!b.overflow)
                b.text.append(static_cast<char*>(e->data), e->data_len);
        }
        return ESP_OK;
    };
    auto client = esp_http_client_init(&c);
    auto err = client ? esp_http_client_perform(client) : ESP_FAIL;
    int code = client ? esp_http_client_get_status_code(client) : 0;
    if (client)
        esp_http_client_cleanup(client);
    if (err != ESP_OK || body.overflow || code != 200) {
        error = code == 401   ? "Invalid OpenWeather key"
                : code == 429 ? "OpenWeather rate limit"
                : code == 404 ? "Location unavailable"
                              : "Weather network or service error";
        return parse("");
    }
    auto j = parse(body.text);
    if (!j)
        error = "Invalid OpenWeather response";
    return j;
}
string lookup(Request r, string& results, string& weather) {
    auto& c = r.settings;
    string error;
    if (r.type == Job::Search || !coord(c.lat, c.lon)) {
        auto locations = fetch("https://api.openweathermap.org/geo/1.0/direct?limit=5&q=" +
                                   escape(c.city + (c.country.empty() ? "" : "," + c.country)) +
                                   "&appid=" + escape(c.key),
                               error);
        if (!locations)
            return error;
        Json list(cJSON_CreateArray(), cJSON_Delete);
        for (auto* p = locations->child; p; p = p->next) {
            double a = num(p, "lat"), b = num(p, "lon");
            if (!coord(a, b) || str(p, "name").empty())
                continue;
            auto* v = cJSON_CreateObject();
            for (auto k : {"name", "country", "state"})
                add(v, k, str(p, k));
            number(v, "lat", a);
            number(v, "lon", b);
            cJSON_AddItemToArray(list.get(), v);
        }
        results = serialize(list.get());
        if (!list->child)
            return "No matching city";
        if (r.type == Job::Search)
            return "City search complete";
        if (list->child->next)
            return "Multiple cities match. Select a search result.";
        c.lat = num(list->child, "lat");
        c.lon = num(list->child, "lon");
    }
    auto data =
        fetch("https://api.openweathermap.org/data/2.5/weather?lat=" + std::to_string(c.lat) +
                  "&lon=" + std::to_string(c.lon) + "&units=" + c.units + "&appid=" + escape(c.key),
              error);
    if (!data)
        return error;
    auto* main = field(data.get(), "main");
    auto* sys = field(data.get(), "sys");
    auto* wind = field(data.get(), "wind");
    auto desc = str(cJSON_GetArrayItem(field(data.get(), "weather"), 0), "description");
    if (desc.empty() || str(data.get(), "name").empty())
        return "Incomplete weather response";
    Json out(cJSON_CreateObject(), cJSON_Delete);
    for (auto k : {"temp", "feels_like", "humidity", "temp_min", "temp_max"}) {
        double v = num(main, k);
        if (!std::isfinite(v))
            return "Incomplete weather response";
        number(out.get(), k, v);
    }
    for (auto k : {"sunrise", "sunset"}) {
        double v = num(sys, k);
        if (!std::isfinite(v))
            return "Incomplete weather response";
        number(out.get(), k, v);
    }
    double t = num(data.get(), "dt"), w = num(wind, "speed");
    if (!std::isfinite(t) || !std::isfinite(w))
        return "Incomplete weather response";
    number(out.get(), "observed_at", t);
    number(out.get(), "wind_speed", w);
    number(out.get(), "latitude", c.lat);
    number(out.get(), "longitude", c.lon);
    add(out.get(), "location", str(data.get(), "name"));
    add(out.get(), "country", str(sys, "country"));
    add(out.get(), "description", desc);
    add(out.get(), "temperature_unit", c.units == "metric" ? "C" : "F");
    add(out.get(), "wind_unit", c.units == "metric" ? "m/s" : "mph");
    weather = serialize(out.get());
    return "Updated";
}
void process_weather() {
    Request r;
    {
        std::lock_guard lock(mu);
        if (!online.load())
            return;
        if (pending.type != Job::None) {
            r = std::move(pending);
            pending = {};
        } else if (cfg.enabled && !cfg.key.empty() && !cfg.city.empty() && now() >= next_fetch) {
            r = {Job::Fetch, cfg, generation};
        } else
            return;
        working = true;
        status = r.type == Job::Search ? "Searching" : "Updating";
    }
    string results, weather;
    auto msg = lookup(r, results, weather);
    std::lock_guard lock(mu);
    working = false;
    if (r.version != generation)
        return;
    status = msg;
    if (!results.empty())
        choices = results;
    if (r.type == Job::Fetch) {
        next_fetch = now() + (weather.empty() ? 90 : 720);
        if (!weather.empty()) {
            cached = weather;
            last_update = time(nullptr) > 1700000000 ? time(nullptr) : 0;
        } else
            cached.clear();
    }
}
void task(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        process_weather();
        bool stop;
        {
            std::lock_guard lock(mu);
            stop = !cfg.automatic && manual_complete.exchange(false);
        }
        if (stop)
            sync(false);
    }
}
}  // namespace
void Init() {
    load();
    if (cfg.enabled)
        status = cfg.key.empty()    ? "API key required"
                 : cfg.city.empty() ? "City required"
                                    : "Waiting for home Wi-Fi";
    setenv("TZ", rule(cfg.zone), 1);
    tzset();
    psa_crypto_init();
    if (xTaskCreate(task, "aura-weather", 8192, nullptr, 1, &worker) != pdPASS)
        worker = nullptr;
}
bool IsOnline() { return online.load(); }
void Online(bool connected) {
    online = connected;
    bool automatic;
    {
        std::lock_guard lock(mu);
        automatic = cfg.automatic;
    }
    sync(connected && automatic);
    if (connected && worker)
        xTaskNotifyGive(worker);
}
bool Authorized(const string& token) {
    if (!hex(token))
        return false;
    auto hash = digest(token);
    std::lock_guard lock(mu);
    if (hash.empty() || hash.size() != cfg.verifier.size())
        return false;
    unsigned diff = 0;
    for (size_t i = 0; i < hash.size(); i++)
        diff |= hash[i] ^ cfg.verifier[i];
    return diff == 0;
}
cJSON* Clock() {
    std::lock_guard lock(mu);
    return clock_locked();
}
cJSON* Weather() {
    std::lock_guard lock(mu);
    if (cfg.enabled && !cached.empty() && now() < next_fetch)
        return parse(cached).release();
    auto* j = cJSON_CreateObject();
    if (worker && cfg.enabled && online)
        xTaskNotifyGive(worker);
    add(j, "status",
        !cfg.enabled          ? "Disabled"
        : !online             ? "Offline"
        : status == "Updated" ? "Awaiting fresh observation"
                              : status);
    return j;
}
cJSON* Snapshot(bool home) {
    std::lock_guard lock(mu);
    auto* j = settings(cfg);
    cJSON_AddBoolToObject(j, "home", home);
    cJSON_AddBoolToObject(j, "key_saved", !cfg.key.empty());
    cJSON_AddBoolToObject(j, "paired", !cfg.verifier.empty());
    add(j, "firmware", esp_app_get_description()->version);
    add(j, "weather_status", status);
    number(j, "last_update", last_update);
    auto* list = cJSON_AddArrayToObject(j, "zones");
    for (auto z : zones)
        cJSON_AddItemToArray(list, cJSON_CreateString(z.name));
    cJSON_AddItemToObject(j, "clock", clock_locked());
    cJSON_AddItemToObject(j, "cities", parse(choices).release());
    if (!cached.empty())
        cJSON_AddItemToObject(j, "weather", parse(cached).release());
    auto net = esp_netif_get_handle_from_ifkey(home ? "WIFI_STA_DEF" : "WIFI_AP_DEF");
    esp_netif_ip_info_t ip{};
    char addr[20]{};
    if (net && esp_netif_get_ip_info(net, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, addr, sizeof(addr));
        add(j, "ip", addr);
    }
    return j;
}
const char* Command(const cJSON* j, bool home) {
    const auto action = str(j, "action");
    std::unique_lock lock(mu);
    auto c = cfg;
    if (action == "pair") {
        if (home)
            return "Pair in the provisioning hotspot";
        auto token = str(j, "token");
        if (!hex(token))
            return "Use a generated access code";
        c.verifier = digest(token);
        if (c.verifier.empty())
            return "Pairing unavailable";
    } else if (action == "time") {
        auto z = str(j, "zone");
        if (!rule(z) || !cJSON_IsBool(field(j, "automatic")) || !cJSON_IsBool(field(j, "h24")))
            return "Invalid clock settings";
        c.zone = z;
        c.automatic = flag(j, "automatic");
        c.h24 = flag(j, "h24");
    } else if (action == "sync") {
        if (!online)
            return "Connect to home Wi-Fi to synchronize";
        lock.unlock();
        manual_complete = false;
        sync(true, true);
        return nullptr;
    } else if (action == "weather" || action == "search" || action == "test") {
        if (!cJSON_IsString(field(j, "city")) || !cJSON_IsString(field(j, "country")) ||
            (field(j, "key") && !cJSON_IsString(field(j, "key"))) ||
            (field(j, "clear_key") && !cJSON_IsBool(field(j, "clear_key"))))
            return "Invalid weather settings";
        c.city = str(j, "city");
        c.country = str(j, "country");
        c.units = str(j, "units");
        auto key = str(j, "key");
        if (!key.empty()) {
            if (!hex(key))
                return "Expected a 32-character hexadecimal OpenWeather key";
            c.key = key;
        }
        if (flag(j, "clear_key"))
            c.key.clear();
        if (!clean(c.city, 80) ||
            (!c.country.empty() && (c.country.size() != 2 ||
                                    !std::all_of(c.country.begin(), c.country.end(),
                                                 [](char x) { return x >= 'A' && x <= 'Z'; }))) ||
            (c.units != "metric" && c.units != "imperial"))
            return "Invalid location or units";
        c.lat = num(j, "lat");
        c.lon = num(j, "lon");
        if ((field(j, "lat") || field(j, "lon")) && !coord(c.lat, c.lon))
            return "Invalid coordinates";
        if (action != "weather") {
            if (!online)
                return "Offline. Save settings and validate on home Wi-Fi.";
            if (!worker || working || pending.type != Job::None)
                return "Weather service busy";
            if (c.key.empty() || c.city.empty())
                return "Enter a key and city first";
            pending = {action == "search" ? Job::Search : Job::Test, c, generation};
            status = "Queued";
            xTaskNotifyGive(worker);
            return nullptr;
        }
        if (!cJSON_IsBool(field(j, "enabled")))
            return "Invalid enable setting";
        c.enabled = flag(j, "enabled");
    } else
        return "Unknown action";
    if (!save(c))
        return "Could not save settings";
    cfg = c;
    if (action == "time") {
        setenv("TZ", rule(c.zone), 1);
        tzset();
        lock.unlock();
        sync(online && c.automatic);
    }
    if (action == "weather") {
        ++generation;
        pending = {};
        cached.clear();
        choices = "[]";
        next_fetch = last_update = 0;
        status = !c.enabled       ? "Disabled"
                 : c.key.empty()  ? "API key required"
                 : c.city.empty() ? "City required"
                 : online         ? "Waiting for update"
                                  : "Waiting for home Wi-Fi";
        if (worker)
            xTaskNotifyGive(worker);
    }
    return nullptr;
}
}  // namespace aura
