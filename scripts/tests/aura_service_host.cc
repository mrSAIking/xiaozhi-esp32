// Host integration checks exercise the firmware service against deterministic IDF adapters.
#include <cassert>
#include <iostream>
#include <thread>
#include "aura_search.h"
#include "music_catalog.h"
#include "aura_service.cc"
using namespace aura;
int main() {
    const std::string base="https://raw.githubusercontent.com/example/library/main/list/songs.json";
    std::vector<music::Song> tracks;
    auto testCatalog=[&](const char* file){auto j=parse(std::string(R"({"version":1,"songs":[{"id":"a","title":"Test","artist":"Aura","file":")")+file+R"("}]})");return music::catalog(base,j.get(),tracks);};
    assert(testCatalog("songs/test.mp3") && tracks.front().url==base.substr(0,base.rfind('/')+1)+"songs/test.mp3");
    assert(music::matches(tracks.front(),"aura")&&!music::matches(tracks.front(),"missing"));
    assert(!testCatalog("../test.mp3") && !testCatalog("songs/%2E%2E/test.mp3") && !testCatalog("https://raw.githubusercontent.com/other/library/main/test.mp3"));
    assert(!music::catalog(base,nullptr,tracks));
    for(int n=1;n<15;n++)tracks.push_back({std::to_string(n),"Song "+std::to_string(n),"Fixture","",4});
    for(int offset:{0,6,12}){
        Json page(music::page(tracks,"",offset,6),cJSON_Delete);
        assert(num(page.get(),"total")==15);
        assert(cJSON_GetArraySize(field(page.get(),"songs"))==(offset==12?3:6));
        assert((field(page.get(),"next_offset")!=nullptr)==(offset<12));
    }
    Json filtered(music::page(tracks,"song 14",0,6),cJSON_Delete);
    assert(num(filtered.get(),"total")==1);
    Init();
    auto cmd = [](std::string raw, bool home = false) {
        auto j = parse(raw);
        return Command(j.get(), home);
    };
    const std::string key(32, 'a'), token(32, 'e');
    assert(cmd(R"({"action":"time","zone":"Mars","h24":true,"automatic":true})"));
    assert(cmd(R"({"action":"sync"})"));
    assert(!cmd("{\"action\":\"pair\",\"token\":\"" + token + "\"}"));
    assert(Authorized(token) && !Authorized(key));
    assert(cmd("{\"action\":\"pair\",\"token\":\"" + key + "\"}", true));
    assert(
        !cmd("{\"action\":\"weather\",\"enabled\":true,\"units\":\"metric\",\"country\":\"IN\","
             "\"city\":\"A & B\",\"key\":\"" +
             key + "\"}"));
    auto snap = []() {
        Json s(Snapshot(false), cJSON_Delete);
        return serialize(s.get());
    };
    assert(snap().find(key) == std::string::npos && snap().find(token) == std::string::npos);
    cfg = {};
    load();
    assert(cfg.city == "A & B" && cfg.key == key && Authorized(token));
    {
        Json t(Clock(), cJSON_Delete);
        assert(!flag(t.get(), "synchronized"));
    }
    Online(true);
    geocode = R"([{"name":"A & B","country":"IN","lat":12,"lon":76}])";
    observation =
        R"({"name":"A","sys":{"country":"IN","sunrise":100,"sunset":200},"main":{"temp":25,"feels_like":26,"temp_min":24,"temp_max":27,"humidity":80},"weather":[{"description":"clouds"}],"wind":{"speed":4},"dt":1700000000})";
    process_weather();
    assert(calls == 2 && urls[0].find("A%20%26%20B%2CIN") != std::string::npos);
    Json w(Weather(), cJSON_Delete);
    assert(num(w.get(), "humidity") == 80 && str(w.get(), "temperature_unit") == "C");
    Online(false);
    Json offline(Weather(), cJSON_Delete);
    assert(!field(offline.get(), "temp") && str(offline.get(), "status") == "Offline");
    Online(true);
    process_weather();
    assert(calls == 4);
    mock_us += 721000000;
    reply_code = 401;
    process_weather();
    assert(status == "Invalid OpenWeather key" && cached.empty());
    reply_code = 429;
    mock_us += 91000000;
    process_weather();
    assert(status == "OpenWeather rate limit");
    reply_code = 200;
    geocode = "[]";
    mock_us += 91000000;
    process_weather();
    assert(status == "No matching city");
    geocode = R"([{"name":"A","lat":10,"lon":20},{"name":"A","lat":11,"lon":20}])";
    mock_us += 91000000;
    process_weather();
    assert(status.starts_with("Multiple cities") && cached.empty());
    reply_code = 0;
    mock_us += 91000000;
    process_weather();
    assert(status == "Internet or TLS connection unavailable" && IsOnline());
    reply_code = 200;
    assert(!cmd(R"({"action":"time","zone":"America/New_York","h24":true,"automatic":false})"));
    for (auto [stamp, offset] :
         {std::pair<time_t, const char*>{1704067200, "-0500"}, {1719792000, "-0400"}}) {
        tm t{};
        localtime_r(&stamp, &t);
        char buf[16];
        strftime(buf, sizeof(buf), "%z", &t);
        assert(std::string(buf) == offset);
    }
    assert(!ntp_running);
    assert(!cmd(R"({"action":"sync"})"));
    assert(ntp_running);
    timeval tv{1700000000, 0};
    sync_callback(&tv);
    Json clock(Clock(), cJSON_Delete);
    assert(flag(clock.get(), "synchronized"));
    assert(!cmd(
        R"({"action":"weather","enabled":false,"clear_key":true,"city":"","country":"","units":"imperial"})"));
    assert(cfg.key.empty());
    // Leave HTTPS blocked, then change configuration on another task. The client
    // must remain responsive and the old HTTP response must never enter the cache.
    assert(
        !cmd("{\"action\":\"weather\",\"enabled\":true,\"city\":\"Test\",\"country\":\"IN\","
             "\"lat\":10,\"lon\":70,\"units\":\"metric\",\"key\":\"" +
             key + "\"}"));
    pause_http = true;
    std::thread request(process_weather);
    http_entered.acquire();
    assert(!snap().empty());
    assert(!cmd(R"({"action":"weather","enabled":false,"city":"","country":"","units":"metric"})"));
    http_resume.release();
    request.join();
    assert(cached.empty() && !cfg.enabled);
    assert(!SearchAvailable() && !Search("fixture", true));
    assert(!ConfigureSearch("test-key-one", 1));
    assert(SearchAvailable());
    Online(false);
    int before = calls;
    assert(!Search("fixture", true) && calls == before);
    Online(true);
    observation = R"({"results":[],"answer":"Test observation"})";
    assert(Search("fixture", true) && auth == "Bearer test-key-one");
    assert(urls.back() == "https://api.tavily.com/search");
    assert(!ConfigureSearch("test-key-two", 1));
    reply_code = 401;
    assert(Search("fixture", true).error() == "Invalid Tavily key" &&
           auth == "Bearer test-key-two");
    reply_code = 429;
    assert(Search("fixture", true).error() == "Tavily rate limit");
    reply_code = 0;
    assert(!Search("fixture", true));
    before = calls;
    assert(!ConfigureSearch(nullptr, 0) && !SearchAvailable() && !Search("fixture", true));
    assert(!ConfigureSearch("", 1) && !SearchAvailable() && !Search("fixture", true) &&
           calls == before);
    Json searchState(cJSON_CreateObject(), cJSON_Delete);
    SearchSnapshot(searchState.get(), true);
    assert(serialize(searchState.get()).find("test-key") == std::string::npos);
    std::cout << "Service persistence, masking, pairing, geocoding, cache, failures, SNTP and DST "
                 "passed.\n";
}
