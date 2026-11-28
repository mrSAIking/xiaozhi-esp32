#include "custom_wake_word.h"
#include "audio_service.h"
#include "system_info.h"
#include "assets.h"
#if CONFIG_BOARD_TYPE_ANIME_COMPANION_S3
#include "application.h"
#include "aura_music.h"
#include "aura_voice.h"
#include "voice_policy.h"
#endif

#include <esp_log.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <cJSON.h>

#define TAG "CustomWakeWord"

CustomWakeWord::CustomWakeWord()
    : wake_word_opus_() {
}

CustomWakeWord::~CustomWakeWord() {
#if CONFIG_BOARD_TYPE_ANIME_COMPANION_S3
    voice::Ready(false);
#endif
    if (multinet_model_data_ != nullptr && multinet_ != nullptr) {
        multinet_->destroy(multinet_model_data_);
        multinet_model_data_ = nullptr;
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }

    if (owns_models_ && models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }
}

void CustomWakeWord::ParseWakenetModelConfig() {
    // Read index.json
    auto& assets = Assets::GetInstance();
    void* ptr = nullptr;
    size_t size = 0;
    if (!assets.GetAssetData("index.json", ptr, size)) {
        ESP_LOGE(TAG, "Failed to read index.json");
        return;
    }
    cJSON* root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse index.json");
        return;
    }
    cJSON* multinet_model = cJSON_GetObjectItem(root, "multinet_model");
    if (cJSON_IsObject(multinet_model)) {
        cJSON* language = cJSON_GetObjectItem(multinet_model, "language");
        cJSON* duration = cJSON_GetObjectItem(multinet_model, "duration");
        cJSON* threshold = cJSON_GetObjectItem(multinet_model, "threshold");
        cJSON* commands = cJSON_GetObjectItem(multinet_model, "commands");
        if (cJSON_IsString(language)) {
            language_ = language->valuestring;
        }
        if (cJSON_IsNumber(duration)) {
            duration_ = duration->valueint;
        }
        if (cJSON_IsNumber(threshold)) {
            threshold_ = threshold->valuedouble;
        }
        if (cJSON_IsArray(commands)) {
            for (int i = 0; i < cJSON_GetArraySize(commands); i++) {
                cJSON* command = cJSON_GetArrayItem(commands, i);
                if (cJSON_IsObject(command)) {
                    cJSON* command_name = cJSON_GetObjectItem(command, "command");
                    cJSON* text = cJSON_GetObjectItem(command, "text");
                    cJSON* action = cJSON_GetObjectItem(command, "action");
                    if (cJSON_IsString(command_name) && cJSON_IsString(text) && cJSON_IsString(action)) {
                        commands_.push_back({command_name->valuestring, text->valuestring, action->valuestring});
                        ESP_LOGI(TAG, "Command: %s, Text: %s, Action: %s", command_name->valuestring, text->valuestring, action->valuestring);
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
}


bool CustomWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    commands_.clear();

    if (models_list == nullptr) {
        language_ = "cn";
        models_ = esp_srmodel_init("model");
        owns_models_ = models_ != nullptr;
#ifdef CONFIG_CUSTOM_WAKE_WORD
        threshold_ = CONFIG_CUSTOM_WAKE_WORD_THRESHOLD / 100.0f;
        commands_.push_back({CONFIG_CUSTOM_WAKE_WORD, CONFIG_CUSTOM_WAKE_WORD_DISPLAY, "wake"});
#endif
    } else {
        models_ = models_list;
        ParseWakenetModelConfig();
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }

#if CONFIG_BOARD_TYPE_ANIME_COMPANION_S3
    language_ = "en";
    commands_ = {{"HEY AURA", "Hey Aura", "wake"}, {"STOP THE SONG", "Stop the song", "stop"},
                 {"STOP MUSIC", "Stop music", "stop"}, {"STOP PLAYING", "Stop playing", "stop"}};
    threshold_ = voice::Confidence() / 100.0f;
#endif
    // 初始化 multinet (命令词识别)
    mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, language_.c_str());
#if CONFIG_BOARD_TYPE_ANIME_COMPANION_S3
    if (!mn_name_ || strcmp(mn_name_, "mn6_en") != 0) return false;
#endif
    if (mn_name_ == nullptr) {
        ESP_LOGW(TAG, "Language '%s' multinet not found, falling back to any multinet model", language_.c_str());
        mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, NULL);
    }
    if (mn_name_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize multinet, mn_name is nullptr");
        ESP_LOGI(TAG, "Please refer to https://pcn7cs20v8cr.feishu.cn/wiki/CpQjwQsCJiQSWSkYEvrcxcbVnwh to add custom wake word");
        return false;
    }

    multinet_ = esp_mn_handle_from_name(mn_name_);
    if (!multinet_) return false;
    multinet_model_data_ = multinet_->create(mn_name_, duration_);
    if (!multinet_model_data_) return false;
    multinet_->set_det_threshold(multinet_model_data_, threshold_);
    input_buffer_.reserve(multinet_->get_samp_chunksize(multinet_model_data_));
    if (esp_mn_commands_alloc(multinet_, multinet_model_data_) != ESP_OK) return false;
    for (int i = 0; i < commands_.size(); i++) {
        if (esp_mn_commands_add(i + 1, commands_[i].command.c_str()) != ESP_OK) return false;
    }
    if (esp_mn_commands_update() != nullptr) return false;
#if CONFIG_BOARD_TYPE_ANIME_COMPANION_S3
    voice::Ready(true);
#endif
    
    multinet_->print_active_speech_commands(multinet_model_data_);
#if CONFIG_SEND_WAKE_WORD_DATA
    if (!wake_word_audio_cache_.Initialize(16000 * 2)) {
        ESP_LOGW(TAG, "Wake-word audio upload disabled: PSRAM cache allocation failed");
    }
#endif
    return true;
}

void CustomWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

std::vector<std::string> CustomWakeWord::GetWakeWords() const {
    std::vector<std::string> words;
    if (multinet_model_data_ == nullptr) {
        return words;
    }
    for (const auto& command : commands_) {
        if (command.action == "wake" && !command.text.empty()) {
            words.push_back(command.text);
        }
    }
    return words;
}

void CustomWakeWord::Start() {
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (!running_ && multinet_model_data_) {
        input_buffer_.clear();
        multinet_->clean(multinet_model_data_);
    }
    running_ = true;
}

void CustomWakeWord::Stop() {
    running_ = false;

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    input_buffer_.clear();
}

void CustomWakeWord::Feed(const std::vector<int16_t>& data) {
    FeedSamples(data.data(), data.size(), false);
}

void CustomWakeWord::FeedMono(const int16_t* data, size_t samples) {
    FeedSamples(data, samples, true);
}

void CustomWakeWord::FeedSamples(const int16_t* data, size_t samples, bool mono) {
    if (multinet_model_data_ == nullptr || data == nullptr || samples == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    // Check running state inside lock to avoid TOCTOU race with Stop()
    if (!running_) {
        return;
    }

    // If input channels is 2, we need to fetch the left channel data
    if (!mono && codec_->input_channels() > 1) {
        for (size_t i = 0; i < samples; i += codec_->input_channels()) {
            input_buffer_.push_back(data[i]);
        }
    } else {
        input_buffer_.insert(input_buffer_.end(), data, data + samples);
    }
    
#if CONFIG_BOARD_TYPE_ANIME_COMPANION_S3
    const float requested = voice::Confidence() / 100.0f;
    if (threshold_ != requested) {
        threshold_ = requested;
        multinet_->set_det_threshold(multinet_model_data_, threshold_);
    }
#endif
    int chunksize = multinet_->get_samp_chunksize(multinet_model_data_);
    while (input_buffer_.size() >= chunksize) {
#if CONFIG_SEND_WAKE_WORD_DATA
        wake_word_audio_cache_.Store(input_buffer_.data(), chunksize);
#endif

        esp_mn_state_t mn_state = multinet_->detect(multinet_model_data_, input_buffer_.data());
        
        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_result = multinet_->get_results(multinet_model_data_);
#if CONFIG_BOARD_TYPE_ANIME_COMPANION_S3
            // Do not promote an alternative hypothesis when the best phrase is disabled.
            const int candidates = mn_result && mn_result->num > 0 ? 1 : 0;
#else
            const int candidates = mn_result ? mn_result->num : 0;
#endif
            for (int i = 0; i < candidates && running_; i++) {
                ESP_LOGI(TAG, "Custom wake word detected: command_id=%d, string=%s, prob=%f", 
                        mn_result->command_id[i], mn_result->string, mn_result->prob[i]);
                const int id = mn_result->command_id[i];
                if (id < 1 || static_cast<size_t>(id) > commands_.size()) continue;
                auto& command = commands_[id - 1];
#if CONFIG_BOARD_TYPE_ANIME_COMPANION_S3
                if (!voice::Accept(id, voice::Wake(), voice::MusicCommands(), music::active)) continue;
                if (command.action == "stop") {
                    const auto playback = music::PlaybackId();
                    Application::GetInstance().Schedule([playback, id] {
                        if (voice::MusicCommands() && music::StopPlayback(playback)) {
                            voice::Detected(id);
                            Application::GetInstance().PrepareMusic(true);
                        }
                    });
                    break;
                }
                voice::Detected(id);
#endif
                if (command.action == "wake") {
                    last_detected_wake_word_ = command.text;
                    running_ = false;
                    input_buffer_.clear();
                    
                    if (wake_word_detected_callback_) {
                        wake_word_detected_callback_(last_detected_wake_word_);
                    }
                }
            }
            multinet_->clean(multinet_model_data_);
        } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGD(TAG, "Command word detection timeout, cleaning state");
            multinet_->clean(multinet_model_data_);
        }
        
        if (!running_) {
            break;
        }
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunksize);
    }
}

size_t CustomWakeWord::GetFeedSize() {
    if (multinet_model_data_ == nullptr) {
        return 0;
    }
    return multinet_->get_samp_chunksize(multinet_model_data_);
}

void CustomWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (CustomWakeWord*)arg;
        {
            auto start_time = esp_timer_get_time();
            // Create encoder
            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
            if (encoder_handle == nullptr) {
                ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
                this_->wake_word_audio_cache_.Clear();
                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                this_->wake_word_cv_.notify_all();
                vTaskDelete(nullptr);
                return;
            }
            // Get frame size
            int frame_size = 0;
            int outbuf_size = 0;
            esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
            frame_size = frame_size / sizeof(int16_t);
            // Encode all PCM data
            int packets = 0;
            std::vector<int16_t> in_buffer(frame_size);
            esp_audio_enc_in_frame_t in = {};
            esp_audio_enc_out_frame_t out = {};
            const size_t cached_samples = this_->wake_word_audio_cache_.Size();
            for (size_t offset = 0;
                 offset + static_cast<size_t>(frame_size) <= cached_samples;
                 offset += frame_size) {
                if (this_->wake_word_audio_cache_.Read(
                        offset, in_buffer.data(), frame_size) != static_cast<size_t>(frame_size)) {
                    break;
                }
                std::vector<uint8_t> opus_buf(outbuf_size);
                in.buffer = reinterpret_cast<uint8_t*>(in_buffer.data());
                in.len = frame_size * sizeof(int16_t);
                out.buffer = opus_buf.data();
                out.len = outbuf_size;
                out.encoded_bytes = 0;
                ret = esp_opus_enc_process(encoder_handle, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.emplace_back(opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                    this_->wake_word_cv_.notify_all();
                    packets++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            }
            this_->wake_word_audio_cache_.Clear();
            // Close encoder
            esp_opus_enc_close(encoder_handle);
            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets, (long)((end_time - start_time) / 1000));

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool CustomWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return !wake_word_opus_.empty();
    });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
