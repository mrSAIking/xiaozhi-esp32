#pragma once
#include <cJSON.h>

namespace voice {
bool Wake();
bool SingleTurn();
bool MusicCommands();
int Confidence();
void Detected(int id);
void Ready(bool ready);
void Snapshot(cJSON* state);
const char* Configure(const cJSON* request);
}
