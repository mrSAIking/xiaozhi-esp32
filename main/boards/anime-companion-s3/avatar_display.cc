#include "avatar_display.h"

#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#include <esp_random.h>
#include <cmath>
#include <cstring>

#include "application.h"
#include "companion_index.h"

extern "C" const uint16_t companion_pixels[] asm("_binary_companion_pixels_bin_start");

namespace {
constexpr int kWidth = Ili9488::kWidth, kHeight = Ili9488::kHeight;
constexpr size_t kFrameBytes = kWidth * kHeight * sizeof(uint16_t);
const uint16_t* Pixels(int offset) { return companion_pixels + offset / 2; }

int64_t NextBlink(int64_t ms, const avatar::Mode& mode) {
    return ms + mode.interval[0] + esp_random() % (mode.interval[1] - mode.interval[0] + 1);
}
}  // namespace

AvatarDisplay::AvatarDisplay() : LcdDisplay(nullptr, nullptr, kWidth, kHeight) {
    lvgl_port_cfg_t cfg = ESP_LVGL_PORT_INIT_CONFIG();
    cfg.task_priority = 1;
    cfg.task_affinity = 1;
    cfg.task_max_sleep_ms = 10;
    ESP_ERROR_CHECK(lvgl_port_init(&cfg));
    DisplayLockGuard guard(this);
    constexpr size_t bytes = kWidth * Ili9488::kStripRows * sizeof(uint16_t);
    auto* strip = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(strip ? ESP_OK : ESP_ERR_NO_MEM);
    display_ = lv_display_create(kWidth, kHeight);
    ESP_ERROR_CHECK(display_ ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display_, strip, nullptr, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display_, &panel_driver_);
    lv_display_set_flush_cb(display_, [](lv_display_t* display, const lv_area_t* area,
                                         uint8_t* pixels) {
        auto* panel = static_cast<Ili9488*>(lv_display_get_user_data(display));
        panel->Flush(*area, pixels,
                     lv_draw_buf_width_to_stride(lv_area_get_width(area), LV_COLOR_FORMAT_RGB565));
        lv_display_flush_ready(display);
    });
    lv_timer_set_period(lv_display_get_refr_timer(display_), 10);
}

void AvatarDisplay::SetupUI() {
    if (IsSetupUICalled())
        return;
    LcdDisplay::SetupUI();
    DisplayLockGuard guard(this);
    frame_ = static_cast<uint16_t*>(
        heap_caps_aligned_alloc(64, kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!frame_) {
        ESP_LOGE("Avatar", "No PSRAM framebuffer; retaining setup screen");
        return;
    }
    setup_screen_ = lv_screen_active();
    avatar_screen_ = lv_obj_create(nullptr);
    image_ = lv_image_create(avatar_screen_);
    descriptor_.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor_.header.cf = LV_COLOR_FORMAT_RGB565;
    descriptor_.header.w = kWidth;
    descriptor_.header.h = kHeight;
    descriptor_.header.stride = kWidth * 2;
    descriptor_.data_size = kFrameBytes;
    descriptor_.data = reinterpret_cast<const uint8_t*>(frame_);
    lv_image_set_src(image_, &descriptor_);
    lv_obj_set_pos(image_, 0, 0);
    lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<AvatarDisplay*>(lv_timer_get_user_data(timer))->Tick();
        },
        10, this);
}

void AvatarDisplay::Invalidate(const avatar::Rect& rect) {
    lv_area_t area{rect.x, rect.y, rect.x + rect.w - 1, rect.y + rect.h - 1};
    lv_obj_invalidate_area(image_, &area);
}

void AvatarDisplay::Patch(const avatar::Rect& rect, int offset) {
    auto* dst = frame_ + rect.y * kWidth + rect.x;
    const auto* src = Pixels(offset);
    for (int i = 0; i < rect.h; ++i, dst += kWidth, src += rect.w) {
        std::memcpy(dst, src, rect.w * sizeof(uint16_t));
    }
    Invalidate(rect);
}

void AvatarDisplay::Breathe(const avatar::Mode& mode, float phase) {
    // Center the deformation on the upper torso within each manifest's safe region.
    auto r = mode.torso;
    r.x += r.w / 4;
    r.y += r.h / 12;
    r.w /= 2;
    r.h /= 3;
    const auto* base = Pixels(mode.base);
    for (int y = 0; y < r.h; ++y) {
        const int row = (r.y + y) * kWidth + r.x;
        const float fy = float(y) / (r.h - 1);
        for (int x = 0; x < r.w; ++x) {
            const float fx = float(x) / (r.w - 1);
            const int shift = int(avatar::Displacement(fx, fy, phase) * 16 + .5f);
            const int src = row + x + (shift / 16) * kWidth;
            frame_[row + x] = avatar::Blend565(base[src], base[src + kWidth], shift % 16);
        }
    }
    Invalidate(r);
}

void AvatarDisplay::Tick() {
    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();
    const bool show = state == kDeviceStateIdle || state == kDeviceStateConnecting ||
                      state == kDeviceStateListening || state == kDeviceStateSpeaking ||
                      state == kDeviceStateNotifying;
    if (!show) {
        if (lv_screen_active() == avatar_screen_)
            lv_screen_load(setup_screen_);
        return;
    }
    const auto now_us = esp_timer_get_time();
    const int64_t ms = now_us / 1000;
    const int selected = requested_mode_.load();
    const auto& mode = avatar::modes[selected];
    if (current_mode_ != selected) {
        std::memcpy(frame_, Pixels(mode.base), kFrameBytes);
        current_mode_ = selected;
        eye_ = mouth_ = 0;
        blink_at_ = NextBlink(ms, mode);
        mouth_at_ = breath_at_ = ms;
        lv_obj_invalidate(image_);
    }
    if (lv_screen_active() != avatar_screen_)
        lv_screen_load(avatar_screen_);
    int eye = 0;
    if (ms >= blink_at_) {
        const int64_t elapsed = ms - blink_at_;
        eye = avatar::EyeFrame(static_cast<int>(std::min<int64_t>(elapsed, 1000)), mode.holds);
        if (eye == 0)
            blink_at_ = NextBlink(ms, mode);
    }
    if (eye != eye_) {
        Patch(mode.eyes, mode.eye_offsets[eye]);
        eye_ = eye;
    }
    const bool speaking =
        (state == kDeviceStateSpeaking && !app.IsSpeechAborted()) || state == kDeviceStateNotifying;
    if (ms >= mouth_at_ || (!speaking && mouth_)) {
        const int mouth =
            speaking
                ? avatar::MouthFrame(app.GetAudioService().GetPlaybackRms(now_us), mode.thresholds)
                : 0;
        if (mouth != mouth_) {
            Patch(mode.mouth, mode.mouth_offsets[mouth]);
            mouth_ = mouth;
        }
        mouth_at_ = ms + mode.speech_ms;
    }
    if (ms >= breath_at_) {
        const float phase = .5f - .5f * std::cos(float(ms % 3600) * (6.2831853f / 3600.f));
        Breathe(mode, phase);
        breath_at_ = ms + 90;
    }
}
