#pragma once
#include <atomic>
#include <string>
#include "audio_service.h"
namespace music {
using Update = void (*)(const char*);
const char* Stream(const std::string& url, AudioService& audio, const std::atomic<bool>& cancel,
                   Update update);
const char* Manifest(const std::string& url, std::string& data, const std::atomic<bool>& cancel);
}  // namespace music
