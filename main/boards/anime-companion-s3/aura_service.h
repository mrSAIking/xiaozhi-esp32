#pragma once
#include <cJSON.h>
#include <string>
namespace aura {
void Init();
bool IsOnline();
void Network(bool connected);
void StartControl();
void Online(bool connected);
void StopHome();
cJSON* Snapshot(bool home);
cJSON* Clock();
cJSON* Weather();
const char* Command(const cJSON* request, bool home);
bool Authorized(const std::string& token);
}  // namespace aura
