#pragma once
#include <cJSON.h>
#include <atomic>
#include <string>
namespace music {
struct View {
    std::string title, artist, state;
    int duration = 0;
    unsigned ms = 0;
};
extern std::atomic<bool> active;
bool Stop();
void Init();
void Network(bool connected);
View Player();
cJSON* Status();
cJSON* List(const std::string& query, int offset, int limit);
const char* Command(const cJSON* request);
}  // namespace music
