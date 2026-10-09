
#include "music_http_streamer.h"

#include "music_player.h"
#include "audio_service.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

#include "sdkconfig.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>

static const char* TAG = "MusicHTTP";

namespace {

struct StreamContext {
    MusicPlayer* player = nullptr;
    AudioService* audio = nullptr;
    size_t received_bytes = 0;
    bool decode_failed = false;
};

// Keep song names safe before inserting them into a URL.
// Examples allowed: funksong, song_2, my-music
bool IsValidSongName(const std::string& name) {
    if (name.empty() || name.size() > 48) {
        return false;
    }

    for (unsigned char ch : name) {
        if (!std::isalnum(ch) &&
            ch != '_' &&
            ch != '-') {
            return false;
        }
    }

    return true;
}

// ESP-IDF delivers downloaded data to this callback.
// Nothing is stored as a complete MP3 file.
esp_err_t HttpEventHandler(
    esp_http_client_event_t* event
) {
    if (event == nullptr) {
        return ESP_FAIL;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }

    auto* context =
        static_cast<StreamContext*>(event->user_data);

    if (context == nullptr ||
        context->player == nullptr ||
        context->audio == nullptr) {
        return ESP_FAIL;
    }

    if (context->decode_failed) {
        return ESP_FAIL;
    }

    // Ignore error pages such as HTTP 404.
    if (esp_http_client_get_status_code(event->client)
        != 200) {
        return ESP_OK;
    }

    if (event->data == nullptr ||
        event->data_len <= 0) {
        return ESP_OK;
    }

    const auto* bytes =
        static_cast<const uint8_t*>(event->data);

    const size_t length =
        static_cast<size_t>(event->data_len);

    if (!context->player->DecodeChunk(
            bytes,
            length,
            *context->audio,
            false)) {

        context->decode_failed = true;
        ESP_LOGE(TAG, "Failed to decode MP3 chunk");
        return ESP_FAIL;
    }

    context->received_bytes += length;

    return ESP_OK;
}

} // namespace

bool MusicHttpStreamer::StreamSong(
    const std::string& song_name,
    MusicPlayer& player,
    AudioService& audio_service
) {
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE

    ESP_LOGE(
        TAG,
        "HTTPS certificate bundle is disabled"
    );

    return false;

#else

    if (!IsValidSongName(song_name)) {
        ESP_LOGE(TAG, "Invalid song name");
        return false;
    }

    // Only access our public GitHub music repository.
    // The MP3 file does not need to be embedded in
    // the ESP32 firmware.
    const std::string url =
        "https://raw.githubusercontent.com/"
        "mrSAIking/music/main/" +
        song_name +
        ".mp3";

    ESP_LOGI(
        TAG,
        "Starting song: %s",
        song_name.c_str()
    );

    if (!player.Initialize()) {
        ESP_LOGE(TAG, "Cannot initialize MP3 decoder");
        return false;
    }

    if (!player.ResetForNewSong()) {
        ESP_LOGE(TAG, "Cannot reset MP3 decoder");
        return false;
    }

    StreamContext context;
    context.player = &player;
    context.audio = &audio_service;

    esp_http_client_config_t config = {};

    config.url = url.c_str();

    // Stream in small chunks to reduce RAM usage.
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;

    config.timeout_ms = 15000;

    // Validate GitHub's HTTPS certificate.
    config.crt_bundle_attach =
        esp_crt_bundle_attach;

    config.event_handler = HttpEventHandler;
    config.user_data = &context;

    // Handle ordinary HTTP redirects automatically.
    config.disable_auto_redirect = false;
    config.max_redirection_count = 3;

    config.keep_alive_enable = false;

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == nullptr) {
        ESP_LOGE(TAG, "HTTP client initialization failed");
        return false;
    }

    esp_http_client_set_method(
        client,
        HTTP_METHOD_GET
    );

    esp_http_client_set_header(
        client,
        "Accept",
        "audio/mpeg"
    );

    esp_http_client_set_header(
        client,
        "Accept-Encoding",
        "identity"
    );

    ESP_LOGI(TAG, "Connecting to GitHub music...");

    // The callback receives data progressively.
    // This call blocks until the transfer finishes.
    esp_err_t result =
        esp_http_client_perform(client);

    const int http_status =
        esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "HTTP stream failed: %s",
            esp_err_to_name(result)
        );
        return false;
    }

    if (http_status != 200) {
        ESP_LOGE(
            TAG,
            "GitHub returned HTTP %d",
            http_status
        );
        return false;
    }

    if (context.decode_failed) {
        ESP_LOGE(TAG, "MP3 decoding failed");
        return false;
    }

    if (context.received_bytes == 0) {
        ESP_LOGE(TAG, "The MP3 file was empty");
        return false;
    }

    ESP_LOGI(
        TAG,
        "Download finished: %u bytes",
        static_cast<unsigned>(
            context.received_bytes
        )
    );

    // Tell the decoder the HTTP stream has ended.
    if (!player.DecodeChunk(
            nullptr,
            0,
            audio_service,
            true)) {

        ESP_LOGE(TAG, "Final MP3 decode failed");
        return false;
    }

    // Wait for pending PCM output buffers.
    // Note: this does not independently verify
    // that the I2S DMA hardware has fully drained.
    audio_service.WaitForPlaybackComplete();

    ESP_LOGI(
        TAG,
        "Song playback queue completed"
    );

    return true;

#endif
}
