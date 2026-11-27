#pragma once
#include <cJSON.h>

namespace aura {
void SetSpeakerVolume(int volume);
const char* ApplyAnimation(const cJSON*, bool save);
void AddAppearance(cJSON* state);
const char* ApplyAppearance(const cJSON* request);
void ReturnToStation();
}
