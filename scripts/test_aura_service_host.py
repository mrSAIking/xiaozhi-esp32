"""Compile a desktop test with the same cJSON version resolved by ESP-IDF."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SDK = r'''
#pragma once
#include <semaphore>
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <sys/time.h>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,HTTP_EVENT_ON_DATA=1,pdTRUE=1,pdPASS=1;
inline long long mock_us=1000000;
inline auto esp_timer_get_time(){return mock_us;}
using TaskHandle_t=void*;
inline int xTaskCreate(void(*)(void*),const char*,int,void*,int,TaskHandle_t* h){*h=(void*)1;return 1;}
inline int pdMS_TO_TICKS(int x){return x;}
inline void ulTaskNotifyTake(int,int){} inline void xTaskNotifyGive(void*){}
inline std::string saved;using nvs_handle_t=int;
constexpr int NVS_READWRITE=1,NVS_READONLY=0;
inline int nvs_open(const char*,int,int*h){*h=1;return 0;}inline void nvs_close(int){}
inline int nvs_commit(int){return 0;}
inline int nvs_set_str(int,const char*,const char*s){saved=s;return 0;}
inline int nvs_get_str(int,const char*,char* p,size_t*n){if(saved.empty())return -1;if(p)memcpy(p,saved.c_str(),saved.size()+1);*n=saved.size()+1;return 0;}
struct esp_sntp_config_t{int num_of_servers;const char* servers[2];void(*sync_cb)(timeval*);};
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(s) {1,{s,nullptr},nullptr}
inline void(*sync_callback)(timeval*);
inline int esp_netif_sntp_init(esp_sntp_config_t*c){sync_callback=c->sync_cb;return 0;}
inline void esp_netif_sntp_deinit(){}
struct esp_http_client_event_t{int event_id;void* user_data;int data_len;void* data;};
struct esp_http_client_config_t{const char* url;int timeout_ms,buffer_size,buffer_size_tx;void(*crt_bundle_attach)();bool disable_auto_redirect;void* user_data;int(*event_handler)(esp_http_client_event_t*);};
inline std::binary_semaphore http_entered{0},http_resume{0};inline bool pause_http=false;
inline int calls=0,reply_code=200;inline std::vector<std::string> urls;
inline std::string geocode,observation;
inline void esp_crt_bundle_attach(){}
inline auto esp_http_client_init(esp_http_client_config_t*c){return c;}
inline int esp_http_client_perform(esp_http_client_config_t*c){if(pause_http){pause_http=false;http_entered.release();http_resume.acquire();}calls++;urls.emplace_back(c->url);auto& s=urls.back().find("/direct")!=std::string::npos?geocode:observation;esp_http_client_event_t e{1,c->user_data,(int)s.size(),s.data()};return c->event_handler(&e);}
inline int esp_http_client_get_status_code(void*){return reply_code;}
inline void esp_http_client_cleanup(void*){}
inline void psa_crypto_init(){}
#define PSA_ALG_SHA_256 1
#define PSA_SUCCESS 0
inline int psa_hash_compute(int,const uint8_t*p,size_t n,uint8_t*out,size_t len,size_t* size){for(size_t i=0;i<len;i++)out[i]=p[i%n]^0x42;*size=len;return 0;}
struct esp_netif_ip_info_t{int ip;};inline int* esp_netif_get_handle_from_ifkey(const char*){return nullptr;}
inline int esp_netif_get_ip_info(int*,esp_netif_ip_info_t*){return -1;}inline void esp_ip4addr_ntoa(int*,char*,int){}
struct App{const char* version;};inline App* esp_app_get_description(){static App a{"test"};return &a;}
'''
def main():
    cjson = next((ROOT / 'managed_components').rglob('cJSON.c'))
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        (d/'sdk.h').write_text(SDK)
        for name in ('esp_app_desc esp_crt_bundle esp_http_client esp_netif_sntp esp_netif esp_timer freertos/FreeRTOS freertos/task nvs psa/crypto').split():
            h = d/(name+'.h')
            h.parent.mkdir(exist_ok=True)
            h.write_text('#include "sdk.h"')
        subprocess.run(['gcc','-c',cjson,'-o',d/'json.o'],check=True)
        subprocess.run(['g++','-std=c++20','-pthread','-I'+str(d),'-I'+str(cjson.parent),
                        '-I'+str(ROOT/'main/boards/anime-companion-s3'),ROOT/'scripts/tests/aura_service_host.cc',d/'json.o','-o',d/'check'],check=True)
        subprocess.run([d/'check'],check=True)
if __name__=='__main__':
    main()

