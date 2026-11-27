"""Run production settings transitions with a deterministic radio, codec and HTTP lifecycle."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def method(file, signature, end="\n}"):
    source = (ROOT / file).read_text(encoding="utf-8")
    start = source.index(signature)
    return source[start:source.index(end, start) + len(end)]


def build_and_run(source):
    include = str(ROOT / "main/boards/anime-companion-s3")
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "test.cc"
        exe = Path(tmp) / "host-test.exe"
        src.write_text(source, encoding="utf-8")
        if shutil.which("c++"):
            args = ["c++", "-std=c++17", "-I" + include, str(src), "-o", str(exe)]
        else:
            args = ["cl", "/nologo", "/EHsc", "/std:c++17", "/I" + include, str(src), "/Fe:" + str(exe)]
        subprocess.run(args, cwd=tmp, check=True)
        subprocess.run([str(exe)], check=True, timeout=10)


@unittest.skipUnless(shutil.which("c++") or shutil.which("cl"), "requires a host C++ compiler")
class SettingsTransitions(unittest.TestCase):
    def test_volume_hold_and_reboot_restore_mute(self):
        codec = "main/audio/audio_codec.cc"
        source = """
#include <cassert>
#include <atomic>
#include "control_button.h"
#define CONFIG_BOARD_TYPE_ANIME_COMPANION_S3 1
#define ESP_LOGI(...)
#define ESP_LOGW(...)
int stored=12,writes=0;
struct Settings {
    Settings(const char*,bool){}
    int GetInt(const char*,int){return stored;}
    void SetInt(const char*,int x){stored=x;++writes;}
};
struct AudioCodec {
    std::atomic<int> output_volume_{70};
    int output_volume() const{return output_volume_.load();}
    void Start(); void SetOutputVolume(int);
};
"""
        source += method(codec,"void AudioCodec::Start()") + "\n"
        source += method(codec,"void AudioCodec::SetOutputVolume(") + "\n"
        source += """
int main(){
    AudioCodec codec;codec.Start();
    aura::ControlButton key;
    for(int ms=0; ms<1100; ms+=10){
        auto edge=key.Sample(ms<1000,ms,true);
        if(edge==aura::Edge::Press || edge==aura::Edge::Repeat)
            codec.output_volume_=aura::StepVolume(codec.output_volume(),-1);
        if(edge==aura::Edge::Release) codec.SetOutputVolume(codec.output_volume());
    }
    assert(writes==1 && stored==0);
    AudioCodec reboot;reboot.Start();assert(reboot.output_volume()==0);
}
"""
        build_and_run(source)

    def test_speech_to_hotspot_then_saved_network(self):
        source = r'''
#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#define CONFIG_BOARD_TYPE_ANIME_COMPANION_S3 1
#define CONFIG_USE_HOTSPOT_WIFI_PROVISIONING 1
#define ESP_LOGI(...)
#define ESP_LOGW(...)
#define kAbortReasonNone 0
enum State {kDeviceStateStarting,kDeviceStateIdle,kDeviceStateSpeaking,kDeviceStateListening,
 kDeviceStateNotifying,kDeviceStateConnecting,kDeviceStateWifiConfiguring};
struct Protocol { bool IsAudioChannelOpened(){return true;} void CloseAudioChannel(){}
 void SendStopListening(){} };
struct Audio { bool mic=true,wake=true,flushed=false; void EnableVoiceProcessing(bool x){mic=x;}
 void EnableWakeWordDetection(bool x){wake=x;} void ResetDecoder(){flushed=true;} };
struct Board { static Board& GetInstance(){static Board b;return b;} Board* GetDisplay(){return this;} void CancelConversationText(){} };
struct Application {
 State state=kDeviceStateSpeaking; bool aborted=false,pending_listening_start_=true;
 Audio audio_service_; std::unique_ptr<Protocol> protocol_=std::make_unique<Protocol>();
 static Application& GetInstance(){static Application a;return a;}
 State GetDeviceState(){return state;} void SetDeviceState(State s){state=s;}
 void AbortSpeaking(int){aborted=true;} void StopNotification(){}
 void Schedule(std::function<void()> f){f();}
 bool PrepareForWifiSettings();
};
namespace aura {bool home=true;void StopHome(){home=false;}void Online(bool){}}
struct SsidManager {std::vector<int> networks{1}; auto GetSsidList(){return networks;}
 static SsidManager& GetInstance(){static SsidManager m;return m;}};
struct Settings {Settings(const char*){} std::string GetString(const char*){
 return SsidManager::GetInstance().networks.empty()?"":"saved";}};
struct WifiManager {
 bool ap=false,sta=true,connected=true;int starts=0,rejoins=0;
 std::function<void()> onExit;
 static WifiManager& GetInstance(){static WifiManager w;return w;}
 bool IsConnected(){return connected;} bool IsConfigMode(){return ap;}
 void StopStation(){sta=connected=false;}
 void StartStation(){assert(!ap);sta=true;++rejoins;}
 void StartConfigAp(){assert(!aura::home && !sta && !ap);ap=true;++starts;}
 std::string GetApSsid(){return "Project-Aura-test";} std::string GetApWebUrl(){return "192.168.4.1";}
 void StopConfigAp(){assert(ap);ap=false;onExit();}
};
struct Display {void ShowNotification(const char*,int){}void SystemMessage(const char*,const char*){}};
struct WifiBoard {
 Display display_; bool in_config_mode_=false,reconnect_failed_=false;
 int connect_timer_=0;
 bool IsInWifiConfigMode(){return WifiManager::GetInstance().ap;}
 Display* GetDisplay(){return &display_;}void ShowWifiConfigPrompt(std::string){}
 void StartWifiConfigMode();void EnterWifiConfigMode();void ReturnToStation();
 static void OnWifiConnectTimeout(void*);void TryWifiConnect();
};
const int CONNECT_TIMEOUT_SEC=60;
void esp_timer_start_once(int,int){}void esp_timer_stop(int){}
namespace Lang::Strings {const char* CONNECT_TO_HOTSPOT="";const char* ACCESS_VIA_BROWSER="";}
'''
        board = "main/boards/common/wifi_board.cc"
        source += method("main/application.cc", "bool Application::PrepareForWifiSettings()")
        for name in ("EnterWifiConfigMode", "StartWifiConfigMode", "TryWifiConnect", "OnWifiConnectTimeout"):
            source += "\n" + method(board, "void WifiBoard::" + name + "(")
        source += "\n" + method("main/boards/anime-companion-s3/anime_companion.cc",
                                 "void ReturnToStation() {", "\n    }").replace(
                                     "void ReturnToStation()", "void WifiBoard::ReturnToStation()")
        source += r'''
int main(){
 WifiBoard b; auto& app=Application::GetInstance(); auto& w=WifiManager::GetInstance();
 w.onExit=[&]{b.TryWifiConnect();};
 for(auto initial:{kDeviceStateSpeaking,kDeviceStateListening}){
   app.state=initial;app.protocol_=std::make_unique<Protocol>();aura::home=true;
   b.EnterWifiConfigMode();b.EnterWifiConfigMode();
   assert(w.ap && !w.sta && !aura::home && app.state==kDeviceStateWifiConfiguring);
   assert(!app.protocol_ && !app.audio_service_.mic && app.audio_service_.flushed);
   SsidManager::GetInstance().networks.clear();b.ReturnToStation();assert(w.ap);
   SsidManager::GetInstance().networks.push_back(1);
   b.ReturnToStation();b.ReturnToStation();assert(w.sta && !w.ap);
   WifiBoard::OnWifiConnectTimeout(&b);assert(w.ap && b.reconnect_failed_);
   b.ReturnToStation();w.connected=true;WifiBoard::OnWifiConnectTimeout(&b);assert(!w.ap);
 }
 assert(app.aborted && w.starts==4 && w.rejoins==4);
}
'''
        build_and_run(source)
