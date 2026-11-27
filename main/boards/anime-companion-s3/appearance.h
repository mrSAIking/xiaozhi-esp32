#pragma once
#include <cJSON.h>

namespace aura {
void AddAppearance(cJSON* state);
const char* ApplyAppearance(const cJSON* request);
void ReturnToStation();
}
