#include "music_stream.h"
#include <strings.h>
#include <array>
#include <cstring>
#include <memory>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"
#include "esp_timer.h"
#include "music_catalog.h"
namespace music {
namespace {
void pause() { vTaskDelay(pdMS_TO_TICKS(10)); }
template <class Sink>
const char* download(const std::string& url, bool song, const std::atomic<bool>& cancel,
                     Sink sink) {
    std::string mime;
    esp_http_client_config_t c{};
    c.url = url.c_str();
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.disable_auto_redirect = true;
    c.timeout_ms = 2500;
    c.buffer_size = 2048;
    c.user_data = &mime;
    c.event_handler = [](esp_http_client_event_t* e) -> esp_err_t {
        if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key &&
            !strcasecmp(e->header_key, "Content-Type") && e->header_value)
            *static_cast<std::string*>(e->user_data) =
                fold(std::string(e->header_value, strnlen(e->header_value, 100)));
        return ESP_OK;
    };
    auto h = esp_http_client_init(&c);
    if (!h)
        return "HTTPS allocation failed";
    struct Cleanup {
        esp_http_client_handle_t h;
        ~Cleanup() { esp_http_client_cleanup(h); }
    } cleanup{h};
    if (cancel || esp_http_client_open(h, 0) != ESP_OK)
        return "Cannot connect to library";
    auto length = esp_http_client_fetch_headers(h);
    if (esp_http_client_get_status_code(h) != 200)
        return "Library file unavailable";
    mime = mime.substr(0, mime.find(';'));
    if (song ? mime != "audio/mpeg" && mime != "audio/mp3" && mime != "application/octet-stream"
             : mime != "application/json" && mime != "text/plain" &&
                   mime != "application/octet-stream")
        return "Unexpected file type";
    if (!song && length > 65536)
        return "Catalog exceeds 64 KiB";
    std::vector<char> chunk(2048);
    uint64_t total = 0;
    while (!cancel) {
        int n = esp_http_client_read(h, chunk.data(), chunk.size());
        if (n < 0)
            return "Stream interrupted";
        if (!n)
            return total && esp_http_client_is_complete_data_received(h) &&
                           (length <= 0 || total == uint64_t(length))
                       ? nullptr
                       : "Incomplete file";
        total += n;
        if (!sink(chunk.data(), size_t(n)))
            return "Transfer cancelled or limit exceeded";
    }
    return "Cancelled";
}
constexpr unsigned capacity = 128 * 1024;
struct Pipe {
    const std::string& url;
    uint8_t* data;
    std::atomic<bool> stop{false}, done{false};
    std::atomic<uint32_t> read{0}, written{0};
    const char* error = nullptr;
    unsigned available() { return written.load() - read.load(); }
    static void produce(void* ctx) {
        auto& p = *static_cast<Pipe*>(ctx);
        p.error = download(p.url, true, p.stop, [&](const char* src, size_t n) {
            while (n && !p.stop) {
                auto w = p.written.load();
                auto free = capacity - (w - p.read.load());
                size_t copy = std::min<size_t>({free, n, capacity - w % capacity});
                if (!copy) {
                    pause();
                    continue;
                }
                memcpy(p.data + w % capacity, src, copy);
                p.written = w + copy;
                src += copy;
                n -= copy;
            }
            return n == 0;
        });
        p.done.store(true);
        vTaskDelete(nullptr);
    }
};
struct Decoder {
    void* mp3 = nullptr;
    esp_ae_rate_cvt_handle_t cvt = nullptr;
    int rate = 0;
    std::vector<int16_t> pcm = std::vector<int16_t>(2304);
    ~Decoder() {
        if (cvt)
            esp_ae_rate_cvt_close(cvt);
        if (mp3)
            esp_mp3_dec_close(mp3);
    }
    bool decode(uint8_t* frame, int size, AudioService& audio, bool& produced) {
        esp_audio_dec_in_raw_t raw{};
        raw.buffer = frame;
        raw.len = size;
        esp_audio_dec_out_frame_t out{};
        out.buffer = (uint8_t*)pcm.data();
        out.len = pcm.size() * 2;
        esp_audio_dec_info_t info{};
        if (esp_mp3_dec_decode(mp3, &raw, &out, &info) != ESP_AUDIO_ERR_OK ||
            raw.consumed != size || out.decoded_size > out.len)
            return false;
        if (!out.decoded_size)
            return true;
        if (info.bits_per_sample != 16 || (info.channel != 1 && info.channel != 2) ||
            out.decoded_size % (2 * info.channel) || info.sample_rate < 8000 ||
            info.sample_rate > 48000)
            return false;
        auto count = out.decoded_size / (2 * info.channel);
        if (info.channel == 2)
            for (unsigned i = 0; i < count; i++)
                pcm[i] = (int(pcm[2 * i]) + pcm[2 * i + 1]) / 2;
        if (rate != int(info.sample_rate)) {
            if (cvt) {
                esp_ae_rate_cvt_close(cvt);
                cvt = nullptr;
            }
            rate = info.sample_rate;
            if (rate != 24000) {
                esp_ae_rate_cvt_cfg_t cfg{};
                cfg.src_rate = rate;
                cfg.dest_rate = 24000;
                cfg.channel = 1;
                cfg.bits_per_sample = 16;
                cfg.complexity = 2;
                cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
                if (esp_ae_rate_cvt_open(&cfg, &cvt) != ESP_AE_ERR_OK)
                    return false;
            }
        }
        std::vector<int16_t> samples;
        if (cvt) {
            uint32_t max = 0;
            if (esp_ae_rate_cvt_get_max_out_sample_num(cvt, count, &max) != ESP_AE_ERR_OK ||
                max > 8192)
                return false;
            samples.resize(max);
            auto n = max;
            if (esp_ae_rate_cvt_process(cvt, (esp_ae_sample_t)pcm.data(), count,
                                        (esp_ae_sample_t)samples.data(), &n) != ESP_AE_ERR_OK ||
                n > max)
                return false;
            samples.resize(n);
        } else
            samples.assign(pcm.begin(), pcm.begin() + count);
        if (samples.empty())
            return true;
        produced = true;
        return audio.QueueMusic(std::move(samples));
    }
};
}  // namespace
const char* Manifest(const std::string& url, std::string& data, const std::atomic<bool>& cancel) {
    return download(url, false, cancel, [&](const char* bytes, size_t n) {
        if (data.size() + n > 65536)
            return false;
        data.append(bytes, n);
        return true;
    });
}
const char* Stream(const std::string& url, AudioService& audio, const std::atomic<bool>& cancel,
                   Update update) {
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> storage(
        (uint8_t*)heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), heap_caps_free);
    if (!storage)
        return "PSRAM buffer unavailable";
    Decoder d;
    auto before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (esp_mp3_dec_open(nullptr, 0, &d.mp3) != ESP_AUDIO_ERR_OK)
        return "MP3 decoder unavailable";
    ESP_LOGI("Music", "128 KiB PSRAM ring; MP3 open heap delta %ld bytes",
             (long)before - (long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    Pipe p{url, storage.get()};
    if (xTaskCreate(Pipe::produce, "aura-mp3-net", 8192, &p, 3, nullptr) != pdPASS)
        return "Stream task unavailable";
    bool priming = true, first = true, played = false;
    uint32_t skip = 0;
    std::vector<uint8_t> frame(2048);
    const char* error = nullptr;
    while (!cancel) {
        bool done = p.done.load();
        if (done && p.error) {
            error = p.error;
            break;
        }
        auto n = p.available();
        if (!n && done) {
            error = p.error ? p.error : skip || !played ? "Incomplete MP3" : nullptr;
            break;
        }
        if (priming && n < 16384 && !done) {
            update("Buffering");
            pause();
            continue;
        }
        priming = false;
        if (skip) {
            auto part = std::min<uint32_t>(skip, n);
            p.read += part;
            skip -= part;
            if (!part)
                pause();
            continue;
        }
        auto offset = p.read.load();
        auto copy = [&](size_t count) {
            auto a = std::min<size_t>(capacity - offset % capacity, count);
            memcpy(frame.data(), p.data + offset % capacity, a);
            memcpy(frame.data() + a, p.data, count - a);
        };
        if (n < 10) {
            if (done) {
                error = "Truncated MP3";
                break;
            }
            priming = true;
            continue;
        }
        copy(10);
        if (first && !memcmp(frame.data(), "ID3", 3)) {
            if (frame[3] < 2 || frame[3] > 4 ||
                ((frame[6] | frame[7] | frame[8] | frame[9]) & 128)) {
                error = "Invalid ID3 header";
                break;
            }
            skip = 10 + ((frame[6] << 21) | (frame[7] << 14) | (frame[8] << 7) | frame[9]) +
                   ((frame[3] == 4 && (frame[5] & 16)) ? 10 : 0);
            if (skip > 4 * 1024 * 1024) {
                error = "ID3 tag exceeds limit";
                break;
            }
            first = false;
            continue;
        }
        first = false;
        if (!memcmp(frame.data(), "TAG", 3) && n <= 128) {
            if (!done) {
                pause();
                continue;
            }
            error = n == 128 && played ? p.error : "Invalid MP3 trailer";
            break;
        }
        int bytes = frameBytes(frame.data());
        if (bytes < 4 || bytes > int(frame.size())) {
            error = "Invalid MPEG Layer III frame";
            break;
        }
        if (n < unsigned(bytes)) {
            if (done) {
                error = "Truncated MP3 frame";
                break;
            }
            priming = true;
            continue;
        }
        copy(bytes);
        p.read += bytes;
        if (!d.decode(frame.data(), bytes, audio, played)) {
            error = "MP3 decode or output failed";
            break;
        }
        if (played)
            update("Playing");
    }
    p.stop = true;
    while (!p.done)
        pause();
    return error;
}
}  // namespace music
