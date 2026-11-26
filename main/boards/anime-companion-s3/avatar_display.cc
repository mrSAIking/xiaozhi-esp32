#include "avatar_display.h"

#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#include <esp_random.h>
#include <esp_system.h>
#include <cstring>

#include "application.h"
#include "companion_index.h"

extern "C" const uint16_t companion_pixels[] asm("_binary_companion_pixels_bin_start");

namespace {
constexpr int kWidth = Ili9488::kWidth, kHeight = Ili9488::kHeight;
constexpr size_t kFrameBytes = kWidth * kHeight * sizeof(uint16_t);
constexpr int kBreathBandRows = 16;
constexpr int kPreviewMs = 5000;
const uint16_t* Pixels(int offset) { return companion_pixels + offset / 2; }

int64_t NextBlink(int64_t ms, const avatar::Mode& mode) {
    return ms + mode.interval[0] + esp_random() % (mode.interval[1] - mode.interval[0] + 1);
}
}  // namespace

AvatarDisplay::AvatarDisplay() : LcdDisplay(nullptr, nullptr, kWidth, kHeight) {
    ESP_LOGI("Avatar", "Previous reset: %s", Board::ResetReasonName(esp_reset_reason()));
#if CONFIG_ANIME_PANEL_SELF_TEST
    panel_driver_.RunColorTest();
#endif
    ESP_ERROR_CHECK(heap_caps_check_integrity_all(true) ? ESP_OK : ESP_FAIL);
    ESP_LOGI("Avatar", "Starting LVGL port");
    lvgl_port_cfg_t cfg = ESP_LVGL_PORT_INIT_CONFIG();
    cfg.task_priority = 1;
    cfg.task_affinity = 1;
    cfg.task_max_sleep_ms = 10;
    ESP_ERROR_CHECK(lvgl_port_init(&cfg));
    DisplayLockGuard guard(this);
    const size_t bytes =
        lv_draw_buf_width_to_stride(kWidth, LV_COLOR_FORMAT_RGB565) * Ili9488::kStripRows;
    auto* strip = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(strip ? ESP_OK : ESP_ERR_NO_MEM);
    display_ = lv_display_create(kWidth, kHeight);
    ESP_ERROR_CHECK(display_ ? ESP_OK : ESP_ERR_NO_MEM);
    // SetupUI publishes a complete stock screen before the first refresh.
    auto* refresh = lv_display_get_refr_timer(display_);
    lv_timer_pause(refresh);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display_, strip, nullptr, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display_, this);
    lv_display_set_flush_cb(
        display_, [](lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
            auto* avatar = static_cast<AvatarDisplay*>(lv_display_get_user_data(display));
            const auto* buffer = lv_display_get_buf_active(display);
            ESP_ERROR_CHECK(buffer && buffer->data == pixels ? ESP_OK : ESP_ERR_INVALID_STATE);
            avatar->panel_driver_.Flush(*area, pixels, buffer->header.stride, buffer->data_size);
            avatar->CheckBreathFlush(*area, *buffer);
            lv_display_flush_ready(display);
        });
    lv_display_add_event_cb(
        display_,
        [](lv_event_t* event) {
            auto* avatar = static_cast<AvatarDisplay*>(lv_event_get_user_data(event));
            if (auto callback = avatar->after_refresh_) {
                avatar->after_refresh_ = nullptr;
                Application::GetInstance().Schedule(callback);
            }
        },
        LV_EVENT_REFR_READY, this);
    lv_timer_set_period(refresh, 10);
    ESP_LOGI("Avatar", "LVGL display registered, PSRAM strip=%zu bytes, refresh paused", bytes);
}

void AvatarDisplay::AfterRefresh(void (*callback)()) {
    DisplayLockGuard guard(this);
    after_refresh_ = callback;
    auto* timer = lv_display_get_refr_timer(display_);
    lv_timer_resume(timer);
    lv_timer_ready(timer);
}

bool AvatarDisplay::Lock(int) { return lvgl_port_lock(0); }

void AvatarDisplay::SetupUI() {
    DisplayLockGuard guard(this);
    if (IsSetupUICalled())
        return;
    ESP_LOGI("Avatar", "Creating Xiaozhi setup screen");
    LcdDisplay::SetupUI();
    lv_timer_resume(lv_display_get_refr_timer(display_));
#if CONFIG_ANIME_SETUP_UI_ONLY
    ESP_LOGI("Avatar", "Setup UI only, avatar disabled");
    return;
#endif
    frame_ = static_cast<uint16_t*>(
        heap_caps_aligned_alloc(64, kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!frame_) {
        ESP_LOGE("Avatar", "No PSRAM framebuffer; retaining setup screen");
        return;
    }
    setup_screen_ = lv_screen_active();
    avatar_screen_ = lv_obj_create(nullptr);
    lv_obj_remove_style_all(avatar_screen_);
    lv_obj_remove_flag(avatar_screen_, LV_OBJ_FLAG_SCROLLABLE);
    image_ = lv_image_create(avatar_screen_);
    descriptor_.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor_.header.cf = LV_COLOR_FORMAT_RGB565;
    descriptor_.header.w = kWidth;
    descriptor_.header.h = kHeight;
    descriptor_.header.stride = kWidth * 2;
    descriptor_.data_size = kFrameBytes;
    descriptor_.data = reinterpret_cast<const uint8_t*>(frame_);
    LoadMode(requested_mode_.load(), esp_timer_get_time() / 1000);
    lv_image_set_src(image_, &descriptor_);
    lv_obj_set_pos(image_, 0, 0);
    lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<AvatarDisplay*>(lv_timer_get_user_data(timer))->Tick();
        },
        10, this);
    ESP_LOGI("Avatar", "UI and avatar ready, animations held until the avatar screen is active");
}

bool AvatarDisplay::Invalidate(const avatar::Rect& rect) {
    lv_area_t image;
    lv_obj_get_coords(image_, &image);
    const int x = image.x1 + rect.x, y = image.y1 + rect.y;
    const lv_area_t area{x, y, x + rect.w - 1, y + rect.h - 1};
    return lv_obj_invalidate_area(image_, &area) == LV_RESULT_OK;
}

void AvatarDisplay::Patch(const avatar::Rect& rect, int offset) {
    auto* dst = frame_ + rect.y * kWidth + rect.x;
    const auto* src = Pixels(offset);
    for (int i = 0; i < rect.h; ++i, dst += kWidth, src += rect.w) {
        std::memcpy(dst, src, rect.w * sizeof(uint16_t));
    }
    Invalidate(rect);
}

void AvatarDisplay::LoadMode(int selected, int64_t ms) {
    const auto& mode = avatar::modes[selected];
    std::memcpy(frame_, Pixels(mode.base), kFrameBytes);
    current_mode_ = selected;
    eye_ = mouth_ = 0;
    blink_at_ = NextBlink(ms, mode);
    mouth_at_ = breath_started_ = breath_at_ = ms;
    next_motion_row_ = 0;
    breath_probe_done_ = false;
    probe_x_ = probe_y_ = -1;
    const auto& region = avatar::kBreathingRegions[selected];
    for (int x = 0; x < region.w; ++x) {
        breath_axis_[x] = avatar::BreathX(float(x) / (region.w - 1));
    }
}

int AvatarDisplay::RenderMotionBand(const avatar::Rect& r, int amount, float arm, bool right_arm) {
    const auto* base = Pixels(avatar::modes[current_mode_].base);
    int left = r.w, right = -1, top = 0, bottom = -1;
    for (int y = 0; y < r.h; ++y) {
        const int row = (r.y + y) * kWidth + r.x;
        // Amount is Q6 pixels. Convert to Q4 for RGB565 sampling.
        const auto& torso = avatar::kBreathingRegions[current_mode_];
        const int py = r.y + y;
        const float weight = amount * .25f * avatar::BreathY(float(py - torso.y) / (torso.h - 1));
        const auto mask = avatar::ArmRow(current_mode_, py);
        for (int x = 0; x < r.w; ++x) {
            const int px = r.x + x;
            const int tx = px - torso.x;
            const auto axis = tx >= 0 && tx < torso.w ? breath_axis_[tx] : avatar::BreathAxis{};
            const float spread =
                axis.spread * weight +
                16.f * arm * avatar::ArmWeight(mask, right_arm ? kWidth - 1 - px : px);
            const int dx = int(spread + (spread < 0 ? -.5f : .5f));
            const int dy = int(axis.lift * weight + .5f);
            const uint16_t pixel = avatar::Sample565(base + row + x, kWidth, dx, dy);
            if (frame_[row + x] == pixel)
                continue;
            frame_[row + x] = pixel;
            left = std::min(left, x);
            right = std::max(right, x);
            if (bottom < 0)
                top = y;
            bottom = y;
            if (!breath_probe_done_ && probe_x_ < 0 && pixel != base[row + x]) {
                probe_x_ = r.x + x;
                probe_y_ = r.y + y;
            }
        }
    }
    if (right < left)
        return 0;
    const avatar::Rect dirty{r.x + left, r.y + top, right - left + 1, bottom - top + 1};
    return Invalidate(dirty) ? dirty.w * dirty.h : 0;
}

void AvatarDisplay::CheckBreathFlush(const lv_area_t& area, const lv_draw_buf_t& buffer) {
    if (probe_x_ < 0)
        return;
    lv_area_t image;
    lv_obj_get_coords(image_, &image);
    const int x = image.x1 + probe_x_, y = image.y1 + probe_y_;
    if (x < area.x1 || x > area.x2 || y < area.y1 || y > area.y2)
        return;
    const int index = probe_y_ * kWidth + probe_x_;
    const uint16_t base = Pixels(avatar::modes[current_mode_].base)[index];
    const uint16_t expected = frame_[index];
    if (expected == base) {
        probe_x_ = -1;
        return;
    }
    uint16_t sent;
    std::memcpy(&sent,
                buffer.data + (y - area.y1) * buffer.header.stride + (x - area.x1) * sizeof(sent),
                sizeof(sent));
    if (sent == expected) {
        ESP_LOGI("Avatar",
                 "Breathing pixel reached SPI completion in %s mode (%d,%d): %04x -> %04x",
                 current_mode_ == 0 ? "full_body" : "half_body", x, y, base, sent);
    } else {
        ESP_LOGW("Avatar", "Breathing flush mismatch at (%d,%d): expected %04x, sent %04x", x, y,
                 expected, sent);
    }
    breath_probe_done_ = true;
    probe_x_ = -1;
}

void AvatarDisplay::Tick() {
    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();
    const bool show = state == kDeviceStateIdle || state == kDeviceStateConnecting ||
                      state == kDeviceStateListening || state == kDeviceStateSpeaking ||
                      state == kDeviceStateNotifying;
    const auto now_us = esp_timer_get_time();
    const int64_t ms = now_us / 1000;
    const bool can_preview = state == kDeviceStateStarting ||
                             state == kDeviceStateWifiConfiguring ||
                             state == kDeviceStateActivating;
    if (preview_requested_.exchange(false) && can_preview)
        preview_until_ = ms + kPreviewMs;
    if (!can_preview)
        preview_until_ = 0;
    if (!show && ms >= preview_until_) {
        if (lv_screen_active() == avatar_screen_) {
            probe_x_ = -1;
            lv_screen_load(setup_screen_);
        }
        return;
    }
    const int selected = requested_mode_.load();
    const auto& mode = avatar::modes[selected];
    if (current_mode_ != selected) {
        LoadMode(selected, ms);
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
    bool face_changed = eye != eye_;
    if (face_changed) {
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
            face_changed = true;
            Patch(mode.mouth, mode.mouth_offsets[mouth]);
            mouth_ = mouth;
        }
        mouth_at_ = ms + mode.speech_ms;
    }
    const int amplitude = selected ? CONFIG_ANIME_ARM_HALF_PX : CONFIG_ANIME_ARM_FULL_PX;
    if (face_changed || ms < breath_at_ || (!amplitude && !CONFIG_ANIME_BREATH_AMPLITUDE_PX))
        return;

    const auto& region = avatar::kMotionRegions[selected];
    const int rows = std::min(kBreathBandRows, region.h - next_motion_row_);
    const int64_t elapsed = ms - breath_started_;
    const int breath = avatar::BreathAmount(elapsed, CONFIG_ANIME_BREATH_PERIOD_MS,
                                            CONFIG_ANIME_BREATH_AMPLITUDE_PX);
    int pixels = 0;
    for (int side = 0; side < 2; ++side) {
        const avatar::Rect band{region.x + side * region.w / 2, region.y + next_motion_row_,
                                region.w / 2, rows};
        pixels += RenderMotionBand(
            band, breath, avatar::ArmOffset(elapsed, CONFIG_ANIME_ARM_PERIOD_MS, amplitude, side),
            side);
    }
    next_motion_row_ = (next_motion_row_ + rows) % region.h;
    // Yield between bands. Charge actual invalidated rectangles at the configured wire rate.
    const int wire_us = pixels * 24 / CONFIG_ANIME_SPI_MHZ;
    breath_at_ = ms + std::max(10, wire_us * (speaking ? 3 : 2) / 1000);
}
