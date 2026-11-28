#include "aura_music.h"
#include <ctime>
#include <mutex>
#include "application.h"
#include "aura_service.h"
#include "mcp_server.h"
#include "music_catalog.h"
#include "music_stream.h"
#include "nvs.h"
namespace music {
std::atomic<bool> active{false}, speaker_claimed{false};
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
std::mutex mu;
bool enabled = false, refresh = false, worker_running = false, network = false;
std::atomic<bool> cancel{false}, cancel_manifest{false};
uint32_t generation = 0, catalog_revision = 0, play_id = 0;
std::string url, library = "Unconfigured", state = "Idle", error;
std::vector<Song> songs;
Song selected;
bool pending = false;
time_t synced = 0;
unsigned position = 0;
AudioService& audio() { return Application::GetInstance().GetAudioService(); }
void update(const char* value) {
    std::lock_guard lock(mu);
    if (active && !cancel)
        state = value;
}
void stopLocked() {
    if (active) {
        cancel = true;
        state = "Stopping";
        active = false;
        position = speaker_claimed ? audio().MusicTimeMs() : 0;
        speaker_claimed = false;
        audio().OwnMusicOutput(false);
    }
}
void run(void*);
bool kickLocked() {
    if (worker_running)
        return true;
    worker_running = xTaskCreate(run, "aura-music", 24576, nullptr, 2, nullptr) == pdPASS;
    if (!worker_running)
        library = "Music task allocation failed";
    return worker_running;
}
const char* configure(bool on, const std::string& path) {
    if ((on || !path.empty()) && (root(path).empty() || !path.ends_with(".json")))
        return "Use a raw.githubusercontent.com JSON manifest URL";
    std::lock_guard lock(mu);
    nvs_handle_t h;
    if (nvs_open("aura_music", NVS_READWRITE, &h) != ESP_OK)
        return "Cannot open music settings";
    std::string record = (on ? "1" : "0") + path;
    auto e = nvs_set_str(h, "library", record.c_str());
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK)
        return "Cannot save music settings";
    stopLocked();
    ++generation;
    ++catalog_revision;
    enabled = on;
    url = path;
    songs.clear();
    synced = 0;
    refresh = on;
    cancel_manifest = true;
    library = !on ? "Disabled" : network ? "Refresh queued" : "Awaiting home Wi-Fi";
    if (on && network && !kickLocked())
        return "Settings saved; music task unavailable";
    return nullptr;
}
const char* play(const std::string& name) {
    std::lock_guard lock(mu);
    if (!enabled || url.empty() || !network)
        return "Music unavailable";
    auto device = Application::GetInstance().GetDeviceState();
    if (device != kDeviceStateIdle && device != kDeviceStateSpeaking &&
        device != kDeviceStateListening && device != kDeviceStateNotifying)
        return "Complete device setup before playback";
    if (active || state == "Stopping" || pending)
        return "Stop current playback first";
    const Song* match = nullptr;
    for (auto& s : songs)
        if (s.id == name) {
            match = &s;
            break;
        }
    if (!match)
        for (auto& s : songs)
            if (fold(s.title) == fold(name)) {
                if (match)
                    return "Multiple matching titles. Use the song ID";
                match = &s;
            }
    if (!match)
        return "Song unavailable. Search the catalog for a title or ID";
    if (!kickLocked())
        return "Music task unavailable";
    selected = *match;
    error.clear();
    position = 0;
    pending = true;
    cancel = false;
    active = true;
    state = "Connecting";
    ++play_id;
    Application::GetInstance().Schedule([id = play_id] {
        if (PlaybackId() == id) audio().EnableWakeWordDetection(true);
    });
    return nullptr;
}
void run(void*) {
    for (;;) {
        std::unique_lock lock(mu);
        if (pending) {
            auto track = selected;
            auto id = play_id;
            pending = false;
            if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
                lock.unlock();
                // Give already-playing confirmation audio a bounded chance to finish.
                for (int i = 0; i < 45 && !cancel && !audio().IsPlaybackIdle(); i++)
                    vTaskDelay(pdMS_TO_TICKS(20));
                lock.lock();
            }
            if (!cancel) {
                speaker_claimed = true;
                audio().OwnMusicOutput(true);
                Application::GetInstance().Schedule([id] {
                    {
                        std::lock_guard guard(mu);
                        if (!active || play_id != id)
                            return;
                    }
                    Application::GetInstance().PrepareMusic();
                });
            }
            lock.unlock();
            auto err = cancel ? nullptr : Stream(track.url, audio(), cancel, update);
            if (!err && !cancel) {
                while (!cancel && !audio().IsPlaybackIdle())
                    vTaskDelay(pdMS_TO_TICKS(10));
                // The last I2S write can still be in the DMA descriptors.
                for (int i = 0; i < 12 && !cancel; i++)
                    vTaskDelay(pdMS_TO_TICKS(10));
            }
            lock.lock();
            if (!cancel)
                position = audio().MusicTimeMs();
            audio().OwnMusicOutput(false);
            active = false;
            speaker_claimed = false;
            state = cancel ? "Idle" : err ? "Error" : "Completed";
            error = cancel || !err ? "" : err;
            Application::GetInstance().Schedule([] {
                auto& app = Application::GetInstance();
                if (!active && app.GetDeviceState() == kDeviceStateIdle)
                    app.GetAudioService().EnableWakeWordDetection(true);
            });
        } else if (enabled && network && refresh) {
            auto target = url;
            auto version = generation;
            refresh = false;
            cancel_manifest = false;
            library = "Fetching library";
            lock.unlock();
            std::string bytes;
            auto err = Manifest(target, bytes, cancel_manifest);
            std::vector<Song> found;
            if (!err) {
                Json j(cJSON_ParseWithLengthOpts(bytes.c_str(), bytes.size() + 1, nullptr, true),
                       cJSON_Delete);
                if (bytes.find(char(0)) != std::string::npos || !j ||
                    !catalog(target, j.get(), found))
                    err = "Invalid catalog or song path";
            }
            lock.lock();
            if (generation == version && network) {
                library = err ? err : "Verified";
                if (!err) {
                    songs.swap(found);
                    ++catalog_revision;
                    auto t = time(nullptr);
                    synced = t > 1700000000 ? t : 0;
                }
            }
        } else {
            worker_running = false;
            break;
        }
    }
    vTaskDelete(nullptr);
}
}  // namespace
uint32_t PlaybackId() {
    std::lock_guard lock(mu);
    return active ? play_id : 0;
}
bool StopPlayback(uint32_t id) {
    std::lock_guard lock(mu);
    if (!active || !id || play_id != id) return false;
    stopLocked();
    return true;
}
bool Stop() {
    std::lock_guard lock(mu);
    bool was = active;
    stopLocked();
    return was;
}
void Network(bool on) {
    std::lock_guard lock(mu);
    network = on;
    ++generation;
    if (!on) {
        stopLocked();
        cancel_manifest = true;
    }
    if (enabled) {
        refresh = true;
        library = on ? "Refresh queued" : "Awaiting home Wi-Fi";
        if (on)
            kickLocked();
    }
}
View Player() {
    std::lock_guard lock(mu);
    return {selected.title, selected.artist, state, selected.duration,
            speaker_claimed ? audio().MusicTimeMs() : position};
}
cJSON* Status() {
    std::lock_guard lock(mu);
    auto j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", enabled);
    cJSON_AddBoolToObject(j, "active", active.load());
    cJSON_AddStringToObject(j, "url", url.c_str());
    cJSON_AddStringToObject(j, "library", library.c_str());
    cJSON_AddStringToObject(j, "state", state.c_str());
    cJSON_AddStringToObject(j, "error", error.c_str());
    cJSON_AddStringToObject(j, "title", selected.title.c_str());
    cJSON_AddStringToObject(j, "artist", selected.artist.c_str());
    cJSON_AddNumberToObject(j, "duration", selected.duration);
    cJSON_AddNumberToObject(j, "ms", speaker_claimed ? audio().MusicTimeMs() : position);
    cJSON_AddNumberToObject(j, "count", songs.size());
    cJSON_AddNumberToObject(j, "revision", catalog_revision);
    cJSON_AddNumberToObject(j, "synced", synced);
    return j;
}
cJSON* List(const std::string& query, int offset, int limit) {
    std::lock_guard lock(mu);
    auto* j = page(songs, query, offset, limit);
    if (!enabled || url.empty())
        cJSON_AddStringToObject(j, "status", "Music unavailable");
    return j;
}
const char* Command(const cJSON* j) {
    std::string a = stringField(j, "action");
    if (a == "music_save") {
        auto e = cJSON_GetObjectItemCaseSensitive(j, "enabled");
        if (!cJSON_IsBool(e))
            return "Invalid music setting";
        return configure(cJSON_IsTrue(e), stringField(j, "url"));
    }
    if (a == "music_stop") {
        Stop();
        return nullptr;
    }
    if (a == "music_play")
        return play(stringField(j, "song"));
    if (a == "music_refresh" || a == "music_test") {
        std::lock_guard lock(mu);
        if (!enabled || !network)
            return "Enable music and connect to home Wi-Fi";
        refresh = true;
        library = "Refresh queued";
        return kickLocked() ? nullptr : "Music task unavailable";
    }
    return "Unknown music action";
}
void Init() {
    nvs_handle_t h;
    if (nvs_open("aura_music", NVS_READONLY, &h) == ESP_OK) {
        char record[560]{};
        size_t n = sizeof(record);
        if (nvs_get_str(h, "library", record, &n) == ESP_OK && n >= 2) {
            url = record + 1;
            enabled = record[0] == '1' && !root(url).empty();
            library = enabled ? "Awaiting home Wi-Fi" : "Disabled";
        }
        nvs_close(h);
    }

    auto& m = McpServer::GetInstance();
    m.AddTool(
        "self.music.list_songs",
        "List actual local library songs; preview six by default. For the full list continue with "
        "next_offset until absent.",
        PropertyList({Property("query", kPropertyTypeString, std::string("")).SetMaxLength(128),
                      Property("offset", kPropertyTypeInteger, 0, 0, 256),
                      Property("limit", kPropertyTypeInteger, 6, 1, 12)}),
        [](const PropertyList& p) -> ToolResult {
            return List(p["query"].value<std::string>(), p["offset"].value<int>(),
                        p["limit"].value<int>());
        });
    m.AddTool("self.music.play", "Play a configured GitHub library song by exact title or ID.",
              PropertyList({Property("song", kPropertyTypeString).SetMaxLength(128)}),
              [](const PropertyList& p) -> ToolResult {
                  if (auto e = play(p["song"].value<std::string>()))
                      return std::unexpected(e);
                  return true;
              });
    m.AddTool("self.music.stop", "Stop the local music player.", PropertyList(),
              [](const PropertyList&) -> ToolResult {
                  Stop();
                  return true;
              });
    m.AddTool("self.music.status", "Report actual local library playback and elapsed milliseconds.",
              PropertyList(), [](const PropertyList&) -> ToolResult { return Status(); });
}
}  // namespace music
