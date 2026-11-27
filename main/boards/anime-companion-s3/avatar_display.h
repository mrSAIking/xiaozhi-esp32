#pragma once

#include <atomic>
#include <mutex>
#include "aura_ui.h"
#include "avatar_pose.h"
#include "display/lcd_display.h"
#include "ili9488.h"

class AvatarDisplay final : public LcdDisplay {
public:
    AvatarDisplay();
    void SetupUI() override;
    void SetTheme(Theme* theme) override;
    void SetHalfBody(bool half) { requested_mode_.store(half ? 1 : 0); }
    void SetWhite(bool value) { requested_white_.store(value); }
    void Volume(int percent);
    void SystemMessage(const char* title, const char* message);
    void SetChatMessage(const char* role, const char* content) override;
    void ShowNotification(const char* text, int duration_ms = 3000) override;
    void ShowNotification(const std::string& text, int duration_ms = 3000) override {
        ShowNotification(text.c_str(), duration_ms);
    }
    void SetTextMode(bool enabled);
    void SetConversationText(const char* role, const char* content) override;
    void CancelConversationText() override;
    void ConnectingWifi();
    void ShowProvisioning();
    void SetStatus(const char* status) override;
    void AfterRefresh(void (*callback)());
    void PreviewMode() { preview_requested_.store(true); }

private:
    // Some inherited stock UI methods assume that the recursive lock succeeds.
    bool Lock(int) override;
    void Tick();
    void CreateOverlays();
    void UpdateOverlays(int64_t ms, bool listening, bool active);
    void Patch(const avatar::Rect& rect, int offset);
    void LoadMode(int selected, int64_t ms);
    int Render(avatar::Rect area);
    bool Invalidate(const avatar::Rect& rect);
    void RefreshComplete();
    void (*after_refresh_)() = nullptr;  // Protected by the LVGL port lock.
    Ili9488 panel_driver_;
    lv_obj_t *boot_screen_ = nullptr, *boot_status_ = nullptr, *boot_fill_ = nullptr;
    bool recovery_ = false, activation_prompt_ = false;
    lv_image_dsc_t boot_image_{};
    lv_obj_t* setup_screen_ = nullptr;
    lv_obj_t* avatar_screen_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_image_dsc_t descriptor_{};
    uint16_t* frame_ = nullptr;
    std::atomic<int> requested_mode_{0};
    std::atomic<bool> preview_requested_{false};
    int64_t preview_until_ = 0;
    std::atomic<bool> requested_white_{false};
    bool white_ = false;
    const uint16_t* Pixels(int offset) const;
    lv_obj_t *system_title_ = nullptr, *system_message_ = nullptr;
    lv_obj_t *volume_box_ = nullptr, *volume_label_ = nullptr, *volume_bar_ = nullptr;
    lv_obj_t* notice_ = nullptr;
    int64_t volume_until_ = 0, notice_until_ = 0;
    int current_mode_ = -1;
    lv_obj_t *dot_ = nullptr, *caption_bar_ = nullptr, *caption_label_ = nullptr;
    std::shared_ptr<LvglFont> caption_font_;
    aura::Captions captions_;
    std::mutex captions_mutex_;
    unsigned caption_revision_ = 0;
    int opacity_ = -1, caption_x_ = 0, caption_width_ = 0;
    bool listening_ = false;
    int region_ = 0;
    avatar::Pose pose_{};
    int64_t due_ms_ = 0, origin_ms_ = 0, sweep_start_us_ = 0;
    int64_t pose_start_us_ = 0, stats_start_us_ = 0;
    int frame_cost_ms_ = 0, completed_ = 0;
    unsigned transfer_bytes_ = 0;
    int eye_ = 0, mouth_ = 0;
    int64_t blink_at_ = 0, mouth_at_ = 0;
};
