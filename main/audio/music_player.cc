
#include "music_player.h"

#include "audio_service.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

#include <esp_log.h>

#include "sdkconfig.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_types.h"

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

    // Register Espressif's built-in audio decoders.
    esp_audio_err_t ret = esp_audio_dec_register_default();

    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Audio decoder registration failed: %d", ret);
        return false;
    }

    // Register the streaming/simple decoder parsers.
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

    ESP_LOGI(TAG, "MP3 streaming decoder initialized");

    return true;

#endif
}

void MusicPlayer::CloseResampler() {
    if (resampler_ != nullptr) {
        esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
    }

    resampler_input_rate_ = 0;
}

bool MusicPlayer::PrepareResampler(int source_rate) {
    if (source_rate <= 0) {
        ESP_LOGE(TAG, "Invalid source sample rate");
        return false;
    }

    // No resampling required for native 24 kHz MP3.
    if (source_rate == kSpeakerSampleRate) {
        CloseResampler();
        return true;
    }

    if (resampler_ != nullptr &&
        resampler_input_rate_ == source_rate) {
        return true;
    }

    CloseResampler();

    esp_ae_rate_cvt_cfg_t cfg = {};

    cfg.src_rate = static_cast<uint32_t>(source_rate);
    cfg.dest_rate = kSpeakerSampleRate;
    cfg.channel = 1;
    cfg.bits_per_sample = ESP_AUDIO_BIT16;
    cfg.complexity = 2;
    cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;

    auto ret = esp_ae_rate_cvt_open(&cfg, &resampler_);

    if (resampler_ == nullptr) {
        ESP_LOGE(
            TAG,
            "Resampler initialization failed: %d",
            ret
        );
        return false;
    }

    resampler_input_rate_ = source_rate;

    ESP_LOGI(
        TAG,
        "Resampling from %d Hz to 24000 Hz",
        source_rate
    );

    return true;
}

bool MusicPlayer::ProcessDecodedPcm(
    const uint8_t* data,
    size_t length,
    AudioService& audio_service
) {
    if (data == nullptr || length == 0) {
        return true;
    }

    if (decoder_ == nullptr) {
        ESP_LOGE(TAG, "Decoder is not initialized");
        return false;
    }

    esp_audio_simple_dec_info_t info = {};

    auto ret = esp_audio_simple_dec_get_info(
        decoder_,
        &info
    );

    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Could not read MP3 audio format");
        return false;
    }

    if (info.bits_per_sample != 16) {
        ESP_LOGE(
            TAG,
            "Unsupported PCM bit depth: %u",
            static_cast<unsigned>(info.bits_per_sample)
        );
        return false;
    }

    if (info.channel != 1 && info.channel != 2) {
        ESP_LOGE(
            TAG,
            "Unsupported channel count: %u",
            static_cast<unsigned>(info.channel)
        );
        return false;
    }

    const size_t bytes_per_sample =
        sizeof(int16_t) * info.channel;

    if (length % bytes_per_sample != 0) {
        ESP_LOGE(TAG, "Misaligned decoded PCM buffer");
        return false;
    }

    const size_t frames = length / bytes_per_sample;

    if (frames == 0) {
        return true;
    }

    // Convert decoded PCM to mono.
    //
    // memcpy avoids unaligned int16_t reads.
    std::vector<int16_t> mono(frames);

    if (info.channel == 1) {
        std::memcpy(
            mono.data(),
            data,
            frames * sizeof(int16_t)
        );
    } else {
        for (size_t i = 0; i < frames; ++i) {
            int16_t left = 0;
            int16_t right = 0;

            std::memcpy(
                &left,
                data + i * 4,
                sizeof(int16_t)
            );

            std::memcpy(
                &right,
                data + i * 4 + sizeof(int16_t),
                sizeof(int16_t)
            );

            // Average with 32-bit arithmetic to avoid overflow.
            const int32_t mixed =
                static_cast<int32_t>(left) +
                static_cast<int32_t>(right);

            mono[i] = static_cast<int16_t>(mixed / 2);
        }
    }

    if (info.sample_rate == kSpeakerSampleRate) {
        return audio_service.PushMusicPcm(
            std::move(mono)
        );
    }

    if (!PrepareResampler(
        static_cast<int>(info.sample_rate)
    )) {
        return false;
    }

    if (frames >
        static_cast<size_t>(
            std::numeric_limits<uint32_t>::max()
        )) {
        ESP_LOGE(TAG, "PCM frame count too large");
        return false;
    }

    uint32_t max_output_samples = 0;

    ret = esp_ae_rate_cvt_get_max_out_sample_num(
        resampler_,
        static_cast<uint32_t>(frames),
        &max_output_samples
    );

    if (ret != ESP_AUDIO_ERR_OK ||
        max_output_samples == 0) {
        ESP_LOGE(TAG, "Unable to calculate resampled size");
        return false;
    }

    std::vector<int16_t> resampled(
        max_output_samples
    );

    uint32_t actual_output_samples = max_output_samples;

    ret = esp_ae_rate_cvt_process(
        resampler_,
        reinterpret_cast<esp_ae_sample_t>(
            mono.data()
        ),
        static_cast<uint32_t>(frames),
        reinterpret_cast<esp_ae_sample_t>(
            resampled.data()
        ),
        &actual_output_samples
    );

    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "PCM resampling failed: %d", ret);
        return false;
    }

    if (actual_output_samples > max_output_samples) {
        ESP_LOGE(TAG, "Invalid resampler output size");
        return false;
    }

    resampled.resize(actual_output_samples);

    if (resampled.empty()) {
        return true;
    }

    // This is the same output queue used by Xiaozhi AI.
    return audio_service.PushMusicPcm(
        std::move(resampled)
    );
}

bool MusicPlayer::DecodeChunk(
    const uint8_t* data,
    size_t length,
    AudioService& audio_service,
    bool end_of_stream
) {
    if (decoder_ == nullptr) {
        ESP_LOGE(TAG, "Decoder is not ready");
        return false;
    }

    if (data == nullptr && length != 0) {
        return false;
    }

    if (length >
        static_cast<size_t>(
            std::numeric_limits<uint32_t>::max()
        )) {
        return false;
    }

    // Decoder input is writable, so use a local copy.
    // Future HTTP streaming will supply small chunks.
    std::vector<uint8_t> input;

    if (length != 0) {
        input.assign(data, data + length);
    }

    // Handles most decoded MP3 frames, including stereo.
    // If more space is needed, the decoder will tell us.
    std::vector<uint8_t> output(8192);

    esp_audio_simple_dec_raw_t raw = {};
    raw.buffer = input.empty() ? nullptr : input.data();
    raw.len = static_cast<uint32_t>(input.size());
    raw.eos = end_of_stream;
    raw.frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE;

    // At EOF, permit the decoder to flush buffered data.
    bool first_iteration = true;

    while (raw.len > 0 ||
           (end_of_stream && first_iteration)) {
        first_iteration = false;

        esp_audio_simple_dec_out_t frame = {};
        frame.buffer = output.data();
        frame.len = static_cast<uint32_t>(output.size());

        const uint32_t before = raw.len;

        auto ret = esp_audio_simple_dec_process(
            decoder_,
            &raw,
            &frame
        );

        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (frame.needed_size == 0 ||
                frame.needed_size > 16384) {
                ESP_LOGE(
                    TAG,
                    "Invalid MP3 output buffer request: %u",
                    static_cast<unsigned>(
                        frame.needed_size
                    )
                );
                return false;
            }

            output.resize(frame.needed_size);

            // Retry without advancing the input.
            first_iteration = true;
            continue;
        }

        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "MP3 decode error: %d", ret);
            return false;
        }

        if (frame.decoded_size > frame.len) {
            ESP_LOGE(TAG, "Decoder output overflow");
            return false;
        }

        if (frame.decoded_size > 0) {
            if (!ProcessDecodedPcm(
                frame.buffer,
                frame.decoded_size,
                audio_service
            )) {
                return false;
            }
        }

        if (raw.consumed > before) {
            ESP_LOGE(TAG, "Invalid decoder consumption");
            return false;
        }

        raw.len = before - raw.consumed;

        if (raw.buffer != nullptr) {
            raw.buffer += raw.consumed;
        }

        // Avoid an infinite loop on decoder/parser stall.
        if (raw.consumed == 0) {
            if (raw.len != 0) {
                ESP_LOGE(
                    TAG,
                    "Decoder made no input progress"
                );
                return false;
            }

            break;
        }
    }

    return true;
}

bool MusicPlayer::ResetForNewSong() {
    if (decoder_ == nullptr) {
        return false;
    }

    auto ret = esp_audio_simple_dec_reset(decoder_);

    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to reset decoder: %d", ret);
        return false;
    }

    CloseResampler();

    ESP_LOGI(TAG, "Decoder ready for a new song");

    return true;
}

void MusicPlayer::Deinitialize() {
    CloseResampler();

    if (decoder_ != nullptr) {
        esp_audio_simple_dec_close(decoder_);
        decoder_ = nullptr;
    }
}

bool MusicPlayer::IsReady() const {
    return decoder_ != nullptr;
}
