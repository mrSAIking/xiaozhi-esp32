#pragma once
#include <cJSON.h>
#include <expected>
#include <string>
namespace aura {
std::expected<std::string, std::string> Search(const std::string& query, bool tool);
bool SearchAvailable();
const char* ConfigureSearch(const char* key, int enabled);
const char* SearchCommand(const cJSON* request, bool home);
void SearchSnapshot(cJSON* state, bool diagnostics);
}  // namespace aura
