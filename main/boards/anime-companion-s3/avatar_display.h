#pragma once

#include <atomic>
#include "avatar_math.h"
#include "display/lcd_display.h"
#include "ili9488.h"

class AvatarDisplay final : public LcdDisplay {
public:
    AvatarDisplay();
    void SetupUI() override;
    void SetHalfBody(bool half) { requested_mode_.store(half ? 1 : 0); }

private:
    void Tick();
    void Patch(const avatar::Rect& rect, int offset);
    void Breathe(const avatar::Mode& mode, float phase);
    void Invalidate(const avatar::Rect& rect);
    Ili9488 panel_driver_;
    lv_obj_t* setup_screen_ = nullptr;
    lv_obj_t* avatar_screen_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_image_dsc_t descriptor_{};
    uint16_t* frame_ = nullptr;
    std::atomic<int> requested_mode_{0};
    int current_mode_ = -1;
    int eye_ = 0, mouth_ = 0;
    int64_t blink_at_ = 0, breath_at_ = 0, mouth_at_ = 0;
};
