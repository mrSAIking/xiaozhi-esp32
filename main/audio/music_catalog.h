#pragma once

// Small HTTPS song catalog for ESP32-WROOM-32 without PSRAM.
// GitHub file: https://raw.githubusercontent.com/mrSAIking/music/main/songs.json
// Called only from MCP tool handlers, not during PCM playback.

#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_log.h>

class MusicCatalog {
private:
    struct Song {
        std::string title;
        std::string file;
    };

    struct Download {
        std::string body;
        bool too_large = false;
    };

    static constexpr size_t kMaxJsonBytes = 4096;
    std::vector<Song> songs_;
    bool loaded_ = false;

    static esp_err_t OnHttpEvent(esp_http_client_event_t* event) {
        if (!event) return ESP_FAIL;
        if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;

        auto* download = static_cast<Download*>(event->user_data);
        if (!download || !event->data || event->data_len <= 0) {
            return ESP_FAIL;
        }

        const size_t count = static_cast<size_t>(event->data_len);
        if (count > kMaxJsonBytes - download->body.size()) {
            download->too_large = true;
            return ESP_FAIL;
        }
        download->body.append(static_cast<const char*>(event->data), count);
        return ESP_OK;
    }

    static bool ValidFile(const std::string& file) {
        if (file.empty() || file.size() > 48) return false;
        for (unsigned char ch : file) {
            if (!((ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') ||
                  ch == '_' || ch == '-')) {
                return false;
            }
        }
        return true;
    }

    static std::string Normalize(const std::string& input) {
        std::string result;
        result.reserve(input.size());
        for (unsigned char ch : input) {
            if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
            if ((ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9')) {
                result.push_back(static_cast<char>(ch));
            }
        }
        return result;
    }

    std::string FindMatch(const std::string& requested) const {
        const std::string key = Normalize(requested);
        if (key.empty()) return {};

        // Exact filename/title matches take priority.
        for (const auto& song : songs_) {
            if (key == Normalize(song.file) || key == Normalize(song.title)) {
                return song.file;
            }
        }

        // Permit a shortened title ("Succession" for "Succession Theme")
        // or a spoken prefix ("Funk Criminal" for "Criminal").
        if (key.size() < 4) return {};
        std::string match;
        for (const auto& song : songs_) {
            const std::string title = Normalize(song.title);
            const bool matches =
                title.compare(0, key.size(), key) == 0 ||
                (title.size() >= 5 && key.find(title) != std::string::npos);
            if (matches) {
                if (!match.empty()) return {}; // Ambiguous: do not guess.
                match = song.file;
            }
        }
        return match;
    }

public:
    static MusicCatalog& GetInstance() {
        static MusicCatalog catalog;
        return catalog;
    }

    bool Refresh() {
        static const char* kUrl =
            "https://raw.githubusercontent.com/mrSAIking/music/main/songs.json";

        Download download;
        download.body.reserve(2048);

        esp_http_client_config_t config = {};
        config.url = kUrl;
        config.buffer_size = 512;
        config.buffer_size_tx = 512;
        config.timeout_ms = 8000;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.event_handler = OnHttpEvent;
        config.user_data = &download;
        config.disable_auto_redirect = false;
        config.max_redirection_count = 3;
        config.keep_alive_enable = false;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) return false;

        esp_http_client_set_header(client, "Accept", "application/json");
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        const esp_err_t result = esp_http_client_perform(client);
        const int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (result != ESP_OK || status != 200 || download.too_large) {
            ESP_LOGW("MusicCatalog", "Fetch failed: %s, HTTP=%d, too_large=%d",
                     esp_err_to_name(result), status, download.too_large);
            return false; // Keep the previous catalog, if any.
        }

        cJSON* root = cJSON_Parse(download.body.c_str());
        if (!root) return false;
        cJSON* array = cJSON_GetObjectItemCaseSensitive(root, "songs");
        if (!cJSON_IsArray(array)) {
            cJSON_Delete(root);
            return false;
        }

        std::vector<Song> next;
        next.reserve(16);
        const int count = cJSON_GetArraySize(array);
        if (count < 1 || count > 32) {
            cJSON_Delete(root);
            return false;
        }

        for (int i = 0; i < count; ++i) {
            cJSON* entry = cJSON_GetArrayItem(array, i);
            cJSON* title = cJSON_GetObjectItemCaseSensitive(entry, "title");
            cJSON* file = cJSON_GetObjectItemCaseSensitive(entry, "file");
            if (!cJSON_IsString(title) || !cJSON_IsString(file) ||
                !title->valuestring || !file->valuestring) {
                continue;
            }
            std::string song_title(title->valuestring);
            std::string song_file(file->valuestring);
            if (song_title.empty() || song_title.size() > 80 ||
                !ValidFile(song_file)) {
                continue;
            }
            next.push_back({std::move(song_title), std::move(song_file)});
        }
        cJSON_Delete(root);
        if (next.empty()) return false;

        songs_ = std::move(next);
        loaded_ = true;
        ESP_LOGI("MusicCatalog", "Loaded %u songs from GitHub",
                 static_cast<unsigned>(songs_.size()));
        return true;
    }

    std::string ListSongs() {
        const bool fresh = Refresh();
        if (!loaded_) {
            return "I could not load the song list from GitHub. "
                   "Please check Wi-Fi and try again.";
        }
        std::string response = "Available songs:\n";
        for (const auto& song : songs_) {
            response += "- " + song.title + " (song_name=" + song.file + ")\n";
        }
        if (!fresh) response += "(Showing last successfully loaded list.)";
        return response;
    }

    std::string ResolveSong(const std::string& requested) {
        if (!loaded_) Refresh();
        std::string match = FindMatch(requested);
        if (!match.empty()) return match;
        // Refresh once on a miss, so newly uploaded songs can be played.
        if (loaded_ && Refresh()) match = FindMatch(requested);
        return match;
    }
};
