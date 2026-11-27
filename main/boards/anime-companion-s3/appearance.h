#pragma once
#include <cJSON.h>

namespace aura {
void SetSpeakerVolume(int volume);
void AddAppearance(cJSON* state);
const char* ApplyAppearance(const cJSON* request);
void ReturnToStation();
}
