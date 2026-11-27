#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_AUDIO_ERR_OK = 0, ESP_AE_ERR_OK = 0, pdPASS = 1,
              HTTP_EVENT_ON_HEADER = 1;
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2, ESP_AE_RATE_CVT_PERF_TYPE_SPEED = 0;
#define ESP_LOGI(...)
inline std::atomic<int> allocations{0};
inline void* heap_caps_malloc(size_t n, int) {
    ++allocations;
    return malloc(n);
}
inline void heap_caps_free(void* p) {
    --allocations;
    free(p);
}
inline size_t heap_caps_get_free_size(int) { return 0; }
inline void vTaskDelay(int) { std::this_thread::sleep_for(std::chrono::microseconds(100)); }
inline void vTaskDelete(void*) {}
inline int pdMS_TO_TICKS(int n) { return n; }
inline int xTaskCreate(void (*f)(void*), const char*, int, void* p, int, void*) {
    std::thread(f, p).detach();
    return 1;
}
struct AudioService {
    unsigned frames = 0;
    std::function<void()> on_frame;
    bool QueueMusic(std::vector<int16_t>&& x) {
        assert(!x.empty());
        ++frames;
        if (on_frame)
            on_frame();
        return true;
    }
};
struct esp_http_client_event_t {
    int event_id;
    const char* header_key;
    const char* header_value;
    void* user_data;
};
struct esp_http_client_config_t {
    const char* url;
    void (*crt_bundle_attach)();
    bool disable_auto_redirect;
    int timeout_ms, buffer_size;
    void* user_data;
    int (*event_handler)(esp_http_client_event_t*);
};
struct Connection {
    esp_http_client_config_t cfg;
    size_t pos;
};
using esp_http_client_handle_t = Connection*;
inline std::string content, mime;
inline bool cut = false;
inline int code = 200;
inline void esp_crt_bundle_attach() {}
inline auto esp_http_client_init(esp_http_client_config_t* c) {
    assert(c->disable_auto_redirect && c->crt_bundle_attach);
    ++allocations;
    return new Connection{*c, 0};
}
inline void esp_http_client_cleanup(Connection* h) {
    delete h;
    --allocations;
}
inline int esp_http_client_open(Connection*, int) { return 0; }
inline int esp_http_client_fetch_headers(Connection* h) {
    esp_http_client_event_t e{1, "Content-Type", mime.c_str(), h->cfg.user_data};
    h->cfg.event_handler(&e);
    return content.size();
}
inline int esp_http_client_get_status_code(Connection*) { return code; }
inline int esp_http_client_read(Connection* h, char* p, int n) {
    if (cut && h->pos > 3200)
        return -1;
    auto count = std::min({size_t(n), size_t(431), content.size() - h->pos});
    memcpy(p, content.data() + h->pos, count);
    h->pos += count;
    return count;
}
inline bool esp_http_client_is_complete_data_received(Connection* h) {
    return h->pos == content.size();
}
struct esp_audio_dec_in_raw_t {
    uint8_t* buffer;
    unsigned len, consumed;
};
struct esp_audio_dec_out_frame_t {
    uint8_t* buffer;
    unsigned len, decoded_size;
};
struct esp_audio_dec_info_t {
    int sample_rate, bits_per_sample, channel;
};
inline int esp_mp3_dec_open(void*, int, void** d) {
    ++allocations;
    *d = (void*)1;
    return 0;
}
inline void esp_mp3_dec_close(void*) { --allocations; }
inline int esp_mp3_dec_decode(void*, esp_audio_dec_in_raw_t* r, esp_audio_dec_out_frame_t* o,
                              esp_audio_dec_info_t* i) {
    r->consumed = r->len;
    *i = {48000, 16, 2};
    o->decoded_size = 4608;
    assert(o->len >= 4608);
    auto p = (int16_t*)o->buffer;
    for (int k = 0; k < 2304; k++)
        p[k] = (k & 1) ? -200 : 400;
    return 0;
}
using esp_ae_rate_cvt_handle_t = void*;
using esp_ae_sample_t = void*;
struct esp_ae_rate_cvt_cfg_t {
    int src_rate, dest_rate, channel, bits_per_sample, complexity, perf_type;
};
inline int esp_ae_rate_cvt_open(esp_ae_rate_cvt_cfg_t* c, void** h) {
    assert(c->dest_rate == 24000);
    ++allocations;
    *h = (void*)1;
    return 0;
}
inline void esp_ae_rate_cvt_close(void*) { --allocations; }
inline int esp_ae_rate_cvt_get_max_out_sample_num(void*, unsigned n, unsigned* out) {
    *out = n / 2;
    return 0;
}
inline int esp_ae_rate_cvt_process(void*, void* src, unsigned n, void* dst, unsigned* out) {
    assert(((int16_t*)src)[0] == 100);
    memset(dst, 0, n);
    *out = n / 2;
    return 0;
}
