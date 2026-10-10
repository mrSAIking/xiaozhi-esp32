// Low-memory raw PCM streamer for ESP32-WROOM-32 (no PSRAM).
// GitHub tracks: signed 16-bit little-endian, mono, 24000 Hz, .pcm extension.
// No MP3 decoder, no resampler, no complete-song buffering.

#include "music_http_streamer.h"
#include "music_player.h"
#include "audio_service.h"

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

// 2048 bytes at 24 kHz mono 16-bit = about 42.7 ms of audio.
constexpr size_t kPcmBlockBytes = 2048;

struct StreamContext {
    AudioService* audio = nullptr;
    std::array<uint8_t, kPcmBlockBytes> block{};
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

bool QueuePcmBlock(StreamContext& context) {
    if (context.block_used == 0) return true;
    if ((context.block_used % sizeof(int16_t)) != 0) {
        ESP_LOGE(TAG, "Odd number of PCM bytes: %u", (unsigned)context.block_used);
        return false;
    }

    std::vector<int16_t> pcm(context.block_used / sizeof(int16_t));
    // Both ESP32 and the GitHub .pcm file use little-endian signed 16-bit PCM.
    std::memcpy(pcm.data(), context.block.data(), context.block_used);
    if (!context.audio->PushMusicPcm(std::move(pcm))) {
        ESP_LOGE(TAG, "PCM playback queue rejected audio");
        return false;
    }
    context.block_used = 0;
    return true;
}

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (event == nullptr) return ESP_FAIL;
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;

    auto* context = static_cast<StreamContext*>(event->user_data);
    if (context == nullptr || context->audio == nullptr || context->playback_failed) {
        return ESP_FAIL;
    }

    if (esp_http_client_get_status_code(event->client) != 200) return ESP_OK;
    if (event->data == nullptr || event->data_len <= 0) return ESP_OK;

    const uint8_t* src = static_cast<const uint8_t*>(event->data);
    size_t remaining = static_cast<size_t>(event->data_len);

    while (remaining != 0) {
        const size_t room = context->block.size() - context->block_used;
        const size_t count = remaining < room ? remaining : room;
        std::memcpy(context->block.data() + context->block_used, src, count);
        context->block_used += count;
        context->received_bytes += count;
        src += count;
        remaining -= count;

        if (context->block_used == context->block.size()) {
            if (!QueuePcmBlock(*context)) {
                context->playback_failed = true;
                return ESP_FAIL;
            }
        }
    }
    return ESP_OK;
}

}  // namespace

bool MusicHttpStreamer::StreamSong(
    const std::string& song_name,
    MusicPlayer& player,
    AudioService& audio_service
) {
    // The old signature is kept so other firmware files need minimal changes.
    (void)player;

#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    ESP_LOGE(TAG, "HTTPS certificate bundle is disabled");
    return false;
#else
    if (!IsValidSongName(song_name)) {
        ESP_LOGE(TAG, "Invalid song name");
        return false;
    }

    const std::string url =
        "https://raw.githubusercontent.com/mrSAIking/music/main/" +
        song_name + ".pcm";

    auto context = std::unique_ptr<StreamContext>(new (std::nothrow) StreamContext());
    if (!context) {
        ESP_LOGE(TAG, "Out of memory allocating PCM stream context");
        return false;
    }
    context->audio = &audio_service;

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
    if (client == nullptr) {
        ESP_LOGE(TAG, "HTTP client initialization failed");
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    ESP_LOGI(TAG, "Streaming raw PCM song: %s", song_name.c_str());
    ESP_LOGI(TAG, "Heap before HTTPS: free=%u, largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    const esp_err_t result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);  // Free TLS/HTTP memory before waiting.

    if (result != ESP_OK || status != 200 || context->playback_failed) {
        ESP_LOGE(TAG, "PCM streaming failed: esp=%s, HTTP=%d, audio_error=%d",
            esp_err_to_name(result), status, context->playback_failed);
        return false;
    }
    if (context->received_bytes == 0 || (context->received_bytes % 2) != 0) {
        ESP_LOGE(TAG, "Invalid PCM byte count: %u", (unsigned)context->received_bytes);
        return false;
    }
    if (!QueuePcmBlock(*context)) {
        ESP_LOGE(TAG, "Failed to queue final PCM samples");
        return false;
    }

    audio_service.WaitForPlaybackComplete();
    ESP_LOGI(TAG, "PCM playback queue completed: %u bytes", (unsigned)context->received_bytes);
    return true;
#endif
}
