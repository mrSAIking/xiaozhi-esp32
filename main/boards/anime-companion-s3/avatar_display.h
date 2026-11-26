#pragma once

#include <array>
#include <atomic>
#include "avatar_math.h"
#include "display/lcd_display.h"
#include "ili9488.h"

class AvatarDisplay final : public LcdDisplay {
public:
    AvatarDisplay();
    void SetupUI() override;
    void SetHalfBody(bool half) { requested_mode_.store(half ? 1 : 0); }
    void AfterRefresh(void (*callback)());
    void PreviewMode() { preview_requested_.store(true); }

private:
    // Some inherited stock UI methods assume that the recursive lock succeeds.
    bool Lock(int) override;
    void Tick();
    void Patch(const avatar::Rect& rect, int offset);
    void LoadMode(int selected, int64_t ms);
    int RenderMotionBand(const avatar::Rect& band, int amount, float arm, bool right);
    bool Invalidate(const avatar::Rect& rect);
    void CheckBreathFlush(const lv_area_t& area, const lv_draw_buf_t& buffer);
    void (*after_refresh_)() = nullptr;  // Protected by the LVGL port lock.
    Ili9488 panel_driver_;
    lv_obj_t* setup_screen_ = nullptr;
    lv_obj_t* avatar_screen_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_image_dsc_t descriptor_{};
    uint16_t* frame_ = nullptr;
    std::atomic<int> requested_mode_{0};
    std::atomic<bool> preview_requested_{false};
    int64_t preview_until_ = 0;
    int current_mode_ = -1;
    std::array<avatar::BreathAxis, Ili9488::kWidth> breath_axis_{};
    int next_motion_row_ = 0;
    int64_t breath_started_ = 0;
    bool breath_probe_done_ = false;
    int probe_x_ = -1, probe_y_ = -1;
    int eye_ = 0, mouth_ = 0;
    int64_t blink_at_ = 0, breath_at_ = 0, mouth_at_ = 0;
};
