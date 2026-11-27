#include "application.h"
#include "assets/lang_config.h"
#include "aura_service.h"
#include "avatar_display.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "mcp_server.h"
#include "settings.h"
#include "wifi_board.h"

#include <esp_heap_caps.h>
#include <led_strip.h>

namespace {
constexpr char kAvatarSettings[] = "anime_avatar";
constexpr char kHalfBodyKey[] = "half_body";

void RainbowTask(void* arg) {
    auto strip = static_cast<led_strip_handle_t>(arg);
    constexpr int peak = CONFIG_ANIME_LED_BRIGHTNESS;
    // Six one-second color transitions. The RMT driver clocks the pixel.
    while (true) {
        const unsigned hue = (esp_timer_get_time() / 1000) % 6000;
        const unsigned ramp = (hue % 1000 * peak + 500) / 1000;
        const unsigned down = peak - ramp;
        const unsigned colors[6][3] = {{peak, ramp, 0}, {down, peak, 0}, {0, peak, ramp},
                                       {0, down, peak}, {ramp, 0, peak}, {peak, 0, down}};
        const auto& c = colors[hue / 1000];
        ESP_ERROR_CHECK(led_strip_set_pixel(strip, 0, c[0], c[1], c[2]));
        ESP_ERROR_CHECK(led_strip_refresh(strip));
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
}  // namespace

class AnimeCompanion final : public WifiBoard {
public:
    AnimeCompanion() {
#if CONFIG_ANIME_PANEL_SELF_TEST
        // Sample once after the color test, before installing click handlers.
        // A long hold after reset selects a one-boot silent provisioning comparison.
        skip_provisioning_audio_ = gpio_get_level(pins::boot) == 0;
        if (skip_provisioning_audio_)
            ESP_LOGI("AnimeBoard", "Silent provisioning comparison selected; release BOOT");
#endif
        half_body_ = Settings(kAvatarSettings).GetBool(kHalfBodyKey);
        display_.SetHalfBody(half_body_);
        ESP_LOGI("AnimeBoard", "Restored avatar mode: %s", half_body_ ? "half_body" : "full_body");
        boot_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            } else {
                app.ToggleChatState();
            }
        });
        boot_.OnDoubleClick([this]() {
            Application::GetInstance().Schedule([this]() {
                if (SetAvatarMode(!half_body_) == ESP_OK)
                    display_.PreviewMode();
            });
        });
        led_strip_config_t led{};
        led.strip_gpio_num = pins::rgb;
        led.max_leds = 1;
        led.led_model = LED_MODEL_WS2812;
        led.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
        led_strip_rmt_config_t rmt{};
        rmt.resolution_hz = 10000000;
        led_strip_handle_t strip = nullptr;
        ESP_ERROR_CHECK(led_strip_new_rmt_device(&led, &rmt, &strip));
        ESP_ERROR_CHECK(xTaskCreate(RainbowTask, "rainbow", 3072, strip, 1, nullptr) == pdPASS
                            ? ESP_OK
                            : ESP_ERR_NO_MEM);

        aura::Init();
        aura::StartControl();
        McpServer::GetInstance().AddTool(
            "self.time.get_current", "Read synchronized local date, weekday, time and zone.",
            PropertyList(), [](const PropertyList&) -> ToolResult { return aura::Clock(); });
        McpServer::GetInstance().AddTool(
            "self.weather.get_current",
            "Current weather observation for the configured home city. Report unavailable status "
            "when there is no observation.",
            PropertyList(), [](const PropertyList&) -> ToolResult { return aura::Weather(); });
        McpServer::GetInstance().AddTool(
            "self.avatar.set_text_mode",
            "Enable for Show me text mode, Enable text mode or Show subtitles. Disable for Turn "
            "off text mode, Disable text mode or Hide subtitles.",
            PropertyList({Property("enabled", kPropertyTypeBoolean)}),
            [this](const PropertyList& props) -> ToolResult {
                display_.SetTextMode(props["enabled"].value<bool>());
                return true;
            });
        McpServer::GetInstance().AddTool(
            "self.avatar.set_mode", "Display the character in full_body or half_body mode.",
            PropertyList({Property("mode", kPropertyTypeString).SetMaxLength(16)}),
            [this](const PropertyList& props) -> ToolResult {
                const auto& mode = props["mode"].value<std::string>();
                if (mode != "full_body" && mode != "half_body")
                    return std::unexpected("Use full_body or half_body");
                if (SetAvatarMode(mode == "half_body") != ESP_OK)
                    return std::unexpected("Could not save avatar mode");
                return true;
            });
    }

    Display* GetDisplay() override { return &display_; }
    void StartNetwork() override {
        ESP_ERROR_CHECK(heap_caps_check_integrity_all(true) ? ESP_OK : ESP_FAIL);
        {
            DisplayLockGuard lock(&display_);
            ESP_ERROR_CHECK(lv_mem_test() == LV_RESULT_OK ? ESP_OK : ESP_FAIL);
        }
        ESP_LOGI("AnimeBoard", "ESP heap and LVGL allocator integrity checks passed");
        ESP_LOGI("AnimeBoard", "Starting Wi-Fi, internal free heap=%zu, PSRAM free heap=%zu",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        display_.ConnectingWifi();
        WifiBoard::StartNetwork();
    }
    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex codec(16000, 24000, pins::speaker_bclk, pins::speaker_lrc,
                                         pins::speaker_din, I2S_STD_SLOT_LEFT, pins::mic_sck,
                                         pins::mic_ws, pins::mic_sd, I2S_STD_SLOT_LEFT);
        return &codec;
    }

private:
    void ShowWifiConfigPrompt(const std::string& hint) override {
        // Publish all three UI changes in one refresh. The display task signals completion
        // after its synchronous SPI flushes. PlaySound can wait on the Opus queue, so it
        // must run on the application task after the display lock has been released.
        DisplayLockGuard guard(&display_);
        auto& app = Application::GetInstance();
        app.Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear");
        ESP_LOGI("AnimeBoard",
                 "Provisioning text queued; waiting for display refresh before audio");
        if (skip_provisioning_audio_) {
            display_.AfterRefresh([] {
                ESP_LOGI("AnimeBoard",
                         "Provisioning refresh complete; prompt audio skipped for comparison");
            });
            return;
        }
        display_.AfterRefresh([] {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateWifiConfiguring)
                return;
            ESP_LOGI("AnimeBoard",
                     "Provisioning refresh complete; starting prompt (wake=%d, voice=%d)",
                     app.GetAudioService().IsWakeWordRunning(),
                     app.GetAudioService().IsAudioProcessorRunning());
            app.PlaySound(Lang::Sounds::OGG_WIFICONFIG);
            ESP_LOGI("AnimeBoard", "Provisioning Opus packets submitted");
        });
    }

    // Both BOOT and MCP requests reach this method on the application task.
    esp_err_t SetAvatarMode(bool half_body) {
        if (half_body == half_body_)
            return ESP_OK;
        nvs_handle_t handle;
        esp_err_t err = nvs_open(kAvatarSettings, NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, kHalfBodyKey, half_body);
            if (err == ESP_OK)
                err = nvs_commit(handle);
            nvs_close(handle);
        }
        if (err != ESP_OK) {
            ESP_LOGE("AnimeBoard", "Saving avatar mode failed: %s", esp_err_to_name(err));
            return err;
        }
        half_body_ = half_body;
        display_.SetHalfBody(half_body_);
        ESP_LOGI("AnimeBoard", "Saved avatar mode: %s", half_body_ ? "half_body" : "full_body");
        return ESP_OK;
    }

    Button boot_{pins::boot};
    AvatarDisplay display_;
    bool half_body_ = false;
    bool skip_provisioning_audio_ = false;
};

DECLARE_BOARD(AnimeCompanion);
