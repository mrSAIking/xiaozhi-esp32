
#include "music_player.h"

#include <esp_log.h>
#include "sdkconfig.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"

static const char* TAG = "MusicPlayer";

MusicPlayer::MusicPlayer() = default;

MusicPlayer::~MusicPlayer() {
    Deinitialize();
}

bool MusicPlayer::Initialize() {
    if (decoder_ != nullptr) {
        return true;
    }

#if !CONFIG_AUDIO_DECODER_MP3_SUPPORT
    ESP_LOGE(TAG, "MP3 decoder support is disabled");
    return false;
#else
    // Register the built-in audio decoders.
    esp_audio_err_t ret = esp_audio_dec_register_default();

    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Decoder registration failed: %d", ret);
        return false;
    }

    // Register the simple streaming decoder parsers.
    ret = esp_audio_simple_dec_register_default();

    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Simple decoder registration failed: %d", ret);
        return false;
    }

    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    cfg.use_frame_dec = false;

    ret = esp_audio_simple_dec_open(&cfg, &decoder_);

    if (ret != ESP_AUDIO_ERR_OK || decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder: %d", ret);
        decoder_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
#endif
}

void MusicPlayer::Deinitialize() {
    if (decoder_ != nullptr) {
        esp_audio_simple_dec_close(decoder_);
        decoder_ = nullptr;
    }
}

bool MusicPlayer::IsReady() const {
    return decoder_ != nullptr;
}
