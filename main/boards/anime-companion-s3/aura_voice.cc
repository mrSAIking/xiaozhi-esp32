#include "aura_voice.h"
#include "application.h"
#include "settings.h"
#include <atomic>
#include <mutex>

namespace voice {
namespace {
constexpr int defaults = (65 << 8) | 7;
std::atomic<int>& controls() {
    static std::atomic<int> value{Settings("aura_voice").GetInt("controls", defaults)};
    return value;
}
std::atomic<int> last{0};
std::atomic<bool> ready{false};
constexpr const char* phrases[] = {"None", "Hey Aura", "Stop the song", "Stop music", "Stop playing"};
}
bool Wake() { return controls().load() & 1; }
bool SingleTurn() { return controls().load() & 2; }
bool MusicCommands() { return controls().load() & 4; }
int Confidence() { return controls().load() >> 8; }
void Ready(bool value) { ready = value; }
void Detected(int id) { if (id >= 1 && id <= 4) last = id; }
void Snapshot(cJSON* state) {
    auto* v = cJSON_AddObjectToObject(state, "voice");
    cJSON_AddBoolToObject(v, "wake", Wake());
    cJSON_AddBoolToObject(v, "single", SingleTurn());
    cJSON_AddBoolToObject(v, "interrupt", MusicCommands());
    cJSON_AddNumberToObject(v, "confidence", Confidence());
    cJSON_AddStringToObject(v, "phrase", ready ? phrases[1] : "Unavailable");
    cJSON_AddStringToObject(v, "last", phrases[last.load()]);
    const bool running = ready && Application::GetInstance().GetAudioService().IsWakeWordRunning();
    cJSON_AddStringToObject(v, "status", !ready ? "Model inactive" : running ? "Monitoring" : "Paused");
}
const char* Configure(const cJSON* request) {
    int value = defaults;
    if (!cJSON_IsTrue(cJSON_GetObjectItem(request, "defaults"))) {
        auto w = cJSON_GetObjectItem(request, "wake"), s = cJSON_GetObjectItem(request, "single");
        auto i = cJSON_GetObjectItem(request, "interrupt"), c = cJSON_GetObjectItem(request, "confidence");
        if (!cJSON_IsBool(w) || !cJSON_IsBool(s) || !cJSON_IsBool(i) || !cJSON_IsNumber(c) ||
            c->valuedouble != c->valueint || c->valueint < 40 || c->valueint > 95)
            return "Invalid voice settings";
        value = (c->valueint << 8) | (cJSON_IsTrue(w) ? 1 : 0) |
                (cJSON_IsTrue(s) ? 2 : 0) | (cJSON_IsTrue(i) ? 4 : 0);
    }
    static std::mutex save_mutex;
    std::lock_guard lock(save_mutex);
    nvs_handle_t h;
    if (nvs_open("aura_voice", NVS_READWRITE, &h) != ESP_OK) return "Cannot open voice settings";
    auto err = nvs_set_i32(h, "controls", value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return "Cannot save voice settings";
    controls() = value;
    Application::GetInstance().Schedule([] {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateIdle) app.GetAudioService().EnableWakeWordDetection(true);
    });
    return nullptr;
}
}
