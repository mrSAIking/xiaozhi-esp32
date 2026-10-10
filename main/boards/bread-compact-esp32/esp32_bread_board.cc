
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "display/oled_display.h"

#include <string>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#define TAG "ESP32-MarsbearSupport"

class CompactWifiBoard : public WifiBoard {
private:
    Button boot_button_;
    Button touch_button_;
    Button asr_button_;

    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };

        ESP_ERROR_CHECK(
            i2c_new_master_bus(
                &bus_config,
                &display_i2c_bus_
            )
        );
    }

    void InitializeSsd1306Display() {
        // SSD1306 display configuration.
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(
            esp_lcd_new_panel_io_i2c_v2(
                display_i2c_bus_,
                &io_config,
                &panel_io_
            )
        );

        ESP_LOGI(TAG, "Install SSD1306 driver");

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };

        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(
            esp_lcd_new_panel_ssd1306(
                panel_io_,
                &panel_config,
                &panel_
            )
        );

        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display.
        ESP_ERROR_CHECK(
            esp_lcd_panel_reset(panel_)
        );

        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // Turn on the OLED.
        ESP_LOGI(TAG, "Turning display on");

        ESP_ERROR_CHECK(
            esp_lcd_panel_disp_on_off(
                panel_,
                true
            )
        );

        display_ = new OledDisplay(
            panel_io_,
            panel_,
            DISPLAY_WIDTH,
            DISPLAY_HEIGHT,
            DISPLAY_MIRROR_X,
            DISPLAY_MIRROR_Y
        );
    }

    void InitializeButtons() {
        // Configure built-in LED GPIO.
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << BUILTIN_LED_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };

        gpio_config(&io_conf);

        // BOOT button: toggle AI conversation.
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();

            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }

            gpio_set_level(BUILTIN_LED_GPIO, 1);
            app.ToggleChatState();
        });

        // ASR button: manually invoke Xiaozhi.
        asr_button_.OnClick([this]() {
            std::string wake_word = "你好小智";

            Application::GetInstance().WakeWordInvoke(
                wake_word
            );
        });

        // Touch button: press to listen.
        touch_button_.OnPressDown([this]() {
            gpio_set_level(BUILTIN_LED_GPIO, 1);

            Application::GetInstance().StartListening();
        });

        // Touch button: release to stop listening.
        touch_button_.OnPressUp([this]() {
            gpio_set_level(BUILTIN_LED_GPIO, 0);

            Application::GetInstance().StopListening();
        });
    }

    // Register tools available to the AI.
    void InitializeTools() {
        // Preserve the existing lamp controller.
        static LampController lamp(LAMP_GPIO);

        // Custom MP3 music playback tool.
        McpServer::GetInstance().AddTool(
            "self.music.play",

            "Play a complete MP3 song through the device speaker. "
            "Use this tool when the user asks to play a song. "
            "For example, when the user says 'Play funksong', "
            "call this tool with song_name='funksong'. "
            "Do not include the .mp3 extension. "
            "Songs are streamed from the configured GitHub music repository. "
            "During music playback, AI voice processing is disabled. "
            "When the song finishes, online AI listening resumes automatically.",

            PropertyList({
                Property(
                    "song_name",
                    kPropertyTypeString
                )
            }),

            [](const PropertyList& properties) -> ReturnValue {
                std::string song_name =
                    properties["song_name"].value<std::string>();

                ESP_LOGI(
                    TAG,
                    "AI requested song: %s",
                    song_name.c_str()
                );

                bool started =
                    Application::GetInstance().StartMusic(
                        song_name
                    );

                if (!started) {
                    ESP_LOGW(
                        TAG,
                        "Unable to start song: %s",
                        song_name.c_str()
                    );
                }

                return started;
            }
        );

        
        // Voice-controlled OLED screen modes.
        // Commands: "Face mode" and "Normal mode".
        McpServer::GetInstance().AddTool(
            "self.screen.set_mode",

            "Change the ESP32 OLED display mode. "
            "When the user says 'Face mode' or "
            "'Face-only mode', call this tool with mode='face'. "
            "When the user says 'Normal mode', "
            "call this tool with mode='normal'. "
            "Face mode shows only the animated facial expression. "
            "Normal mode restores the original display layout. "
            "The screen mode is saved across restarts.",

            PropertyList({
                Property(
                    "mode",
                    kPropertyTypeString
                )
            }),

            [this](const PropertyList& properties) -> ReturnValue {
                std::string mode =
                    properties["mode"].value<std::string>();

                auto* oled =
                    dynamic_cast<OledDisplay*>(display_);

                if (oled == nullptr) {
                    ESP_LOGW(TAG, "OLED display unavailable");
                    return false;
                }

                if (mode == "face" || mode == "face-only") {
                    return oled->SetFaceOnlyMode(true);
                }

                if (mode == "normal") {
                    return oled->SetFaceOnlyMode(false);
                }

                ESP_LOGW(
                    TAG,
                    "Unknown OLED mode: %s",
                    mode.c_str()
                );

                return false;
            }
        );

        ESP_LOGI(
            TAG,
            "Custom music and screen MCP tools registered"
        );

    }

public:
    CompactWifiBoard()
        : WifiBoard(),
          boot_button_(BOOT_BUTTON_GPIO),
          touch_button_(TOUCH_BUTTON_GPIO),
          asr_button_(ASR_BUTTON_GPIO) {

        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX

        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK,
            AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS,
            AUDIO_I2S_MIC_GPIO_DIN
        );

#else

        static NoAudioCodecDuplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN
        );

#endif

        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CompactWifiBoard);
