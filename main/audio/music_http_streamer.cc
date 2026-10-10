// Low-allocation PCM streamer for ESP32-WROOM-32 without PSRAM.
// Files: signed 16-bit little-endian, mono, 24000 Hz, .pcm.
// This version reuses one PCM vector and writes directly to the audio codec.
// Pair with the allocation-free NoAudioCodec::Write() change.

#include "music_http_streamer.h"
#include "music_player.h"
#include "audio_service.h"
#include "audio_codec.h"
#include "board.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "sdkconfig.h"
#include <esp_err.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>

static const char* TAG = "MusicHTTP";

namespace {
constexpr size_t kPcmBlockBytes = 2048;

struct StreamContext {
    AudioCodec* codec = nullptr;
    std::array<uint8_t, kPcmBlockBytes> block{};
    // Preallocate once. Reuse this vector for the entire song.
    std::vector<int16_t> pcm;
    size_t block_used = 0;
    size_t received_bytes = 0;
    bool playback_failed = false;
};

bool IsValidSongName(const std::string& name) {
    if (name.empty() || name.size() > 48) return false;
    for (unsigned char ch : name) {
        if (!std::isalnum(ch) && ch != '_' && ch != '-') return false;
    }
    return true;
}

bool WritePcmBlock(StreamContext& context) {
    if (context.block_used == 0) return true;
    if (context.codec == nullptr || (context.block_used % 2) != 0) {
        ESP_LOGE(TAG, "Invalid PCM block or unavailable codec");
        return false;
    }

    // resize() does not allocate: the vector's capacity was set before HTTPS.
    const size_t sample_count = context.block_used / sizeof(int16_t);
    context.pcm.resize(sample_count);
    std::memcpy(context.pcm.data(), context.block.data(), context.block_used);

    if (!context.codec->output_enabled()) {
        context.codec->EnableOutput(true);
    }
    // Blocking I2S output naturally paces HTTP reception. Unlike PushMusicPcm(),
    // this does not create a new vector or AudioTask for every 42 ms of music.
    context.codec->OutputData(context.pcm);
    context.block_used = 0;
    return true;
}

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (!event) return ESP_FAIL;
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;

    auto* context = static_cast<StreamContext*>(event->user_data);
    if (!context || !context->codec || context->playback_failed) return ESP_FAIL;
    if (esp_http_client_get_status_code(event->client) != 200) return ESP_OK;
    if (!event->data || event->data_len <= 0) return ESP_OK;

    const uint8_t* src = static_cast<const uint8_t*>(event->data);
    size_t remaining = static_cast<size_t>(event->data_len);
    while (remaining > 0) {
        const size_t room = context->block.size() - context->block_used;
        const size_t count = remaining < room ? remaining : room;
        std::memcpy(context->block.data() + context->block_used, src, count);
        context->block_used += count;
        context->received_bytes += count;
        src += count;
        remaining -= count;

        if (context->block_used == context->block.size() &&
            !WritePcmBlock(*context)) {
            context->playback_failed = true;
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}
} // namespace

bool MusicHttpStreamer::StreamSong(const std::string& song_name,
                                   MusicPlayer& player,
                                   AudioService& audio_service) {
    (void)player; // PCM does not need an MP3 decoder.
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    ESP_LOGE(TAG, "HTTPS certificate bundle is disabled");
    return false;
#else
    if (!IsValidSongName(song_name)) {
        ESP_LOGE(TAG, "Invalid song name");
        return false;
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    if (!codec || codec->output_sample_rate() != 24000) {
        ESP_LOGE(TAG, "PCM requires a 24000 Hz output codec");
        return false;
    }

    // Ensure leftover AI speech is finished before direct I2S playback.
    audio_service.WaitForPlaybackComplete();

    auto context = std::unique_ptr<StreamContext>(
        new (std::nothrow) StreamContext());
    if (!context) {
        ESP_LOGE(TAG, "Out of memory creating music context");
        return false;
    }
    context->codec = codec;
    try {
        context->pcm.reserve(kPcmBlockBytes / sizeof(int16_t));
        context->pcm.resize(kPcmBlockBytes / sizeof(int16_t));
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "Out of memory allocating reusable PCM buffer");
        return false;
    }

    const std::string url =
        "https://raw.githubusercontent.com/mrSAIking/music/main/" + song_name + ".pcm";

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.buffer_size = 512;
    config.buffer_size_tx = 512;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = HttpEventHandler;
    config.user_data = context.get();
    config.disable_auto_redirect = false;
    config.max_redirection_count = 3;
    config.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client initialization failed");
        return false;
    }
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    ESP_LOGI(TAG, "Streaming raw PCM song with reusable buffer: %s", song_name.c_str());
    ESP_LOGI(TAG, "Heap before HTTPS: free=%u, largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    const esp_err_t result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (result != ESP_OK || status != 200 || context->playback_failed) {
        ESP_LOGE(TAG, "PCM streaming failed: esp=%s, HTTP=%d, audio_error=%d",
                 esp_err_to_name(result), status, context->playback_failed);
        return false;
    }
    if (context->received_bytes == 0 || (context->received_bytes % 2) != 0) {
        ESP_LOGE(TAG, "Invalid PCM byte count: %u", (unsigned)context->received_bytes);
        return false;
    }
    if (!WritePcmBlock(*context)) {
        ESP_LOGE(TAG, "Final PCM block failed");
        return false;
    }

    ESP_LOGI(TAG, "PCM direct playback finished: %u bytes",
             (unsigned)context->received_bytes);
    return true;
#endif
}
