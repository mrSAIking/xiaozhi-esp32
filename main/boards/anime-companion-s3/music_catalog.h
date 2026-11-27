#pragma once
#include <cJSON.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>
namespace music {
struct Song {
    std::string id, title, artist, url;
    int duration = 0;
};
inline constexpr const char* raw = "https://raw.githubusercontent.com/";
inline bool pathOK(std::string_view s) {
    if (s.empty() || s.size() > 512)
        return false;
    while (!s.empty()) {
        auto end = s.find('/');
        auto part = s.substr(0, end);
        if (part.empty() || part == "." || part == "..")
            return false;
        for (size_t i = 0; i < part.size(); ++i) {
            unsigned char c = part[i];
            if (c == '%' && part.substr(i, 3) == "%20") {
                i += 2;
                continue;
            }
            if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~'))
                return false;
        }
        if (end == s.npos)
            return true;
        s.remove_prefix(end + 1);
    }
    return false;
}
inline std::string root(const std::string& url) {
    if (!url.starts_with(raw))
        return {};
    std::string_view s(url);
    s.remove_prefix(std::char_traits<char>::length(raw));
    if (!pathOK(s))
        return {};
    size_t pos = 0;
    for (int i = 0; i < 3; i++) {
        pos = s.find('/', pos);
        if (pos == s.npos)
            return {};
        pos++;
    }
    return std::string(raw) + std::string(s.substr(0, pos));
}
inline const char* stringField(const cJSON* j, const char* key) {
    auto* v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
inline bool clean(const std::string& v, size_t max) {
    return !v.empty() && v.size() <= max &&
           std::none_of(v.begin(), v.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
inline bool catalog(const std::string& url, const cJSON* j, std::vector<Song>& out) {
    auto origin = root(url);
    auto* songs = cJSON_GetObjectItemCaseSensitive(j, "songs");
    if (origin.empty() ||
        cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "version")) != 1 ||
        !cJSON_IsArray(songs) || cJSON_GetArraySize(songs) > 256)
        return false;
    std::vector<Song> parsed;
    for (auto* s = songs->child; s; s = s->next) {
        Song t{stringField(s, "id"), stringField(s, "title"), stringField(s, "artist"),
               stringField(s, "file")};
        if (!clean(t.id, 64) || !clean(t.title, 128) || !clean(t.artist, 96) ||
            !t.url.ends_with(".mp3"))
            return false;
        if (!t.url.starts_with(raw)) {
            if (!pathOK(t.url))
                return false;
            t.url = url.substr(0, url.rfind('/') + 1) + t.url;
        }
        if (root(t.url) != origin ||
            std::any_of(parsed.begin(), parsed.end(), [&](auto& old) { return old.id == t.id; }))
            return false;
        if (auto* n = cJSON_GetObjectItemCaseSensitive(s, "duration_seconds")) {
            double v = cJSON_GetNumberValue(n);
            if (!std::isfinite(v) || v < 0 || v > 86400 || std::floor(v) != v)
                return false;
            t.duration = int(v);
        }
        parsed.push_back(std::move(t));
    }
    out.swap(parsed);
    return true;
}
inline std::string fold(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return s;
}
inline bool matches(const Song& s, const std::string& folded) {
    return fold(s.title).find(folded) != std::string::npos ||
           fold(s.artist).find(folded) != std::string::npos;
}
inline int frameBytes(const uint8_t* h) {
    if (h[0] != 255 || (h[1] & 0xe0) != 0xe0 || (h[1] & 6) != 2)
        return 0;
    int ver = (h[1] >> 3) & 3, br = h[2] >> 4, sr = (h[2] >> 2) & 3;
    if (ver == 1 || !br || br == 15 || sr == 3)
        return 0;
    constexpr int rates[] = {44100, 48000, 32000};
    constexpr int high[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    constexpr int low[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    int hz = rates[sr] / (ver == 3 ? 1 : ver == 2 ? 2 : 4);
    return (ver == 3 ? 144000 * high[br] : 72000 * low[br]) / hz + ((h[2] >> 1) & 1);
}
}  // namespace music
