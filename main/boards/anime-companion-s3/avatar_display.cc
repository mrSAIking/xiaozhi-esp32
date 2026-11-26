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
            avatar->transfer_bytes_ += 3u * lv_area_get_width(area) * lv_area_get_height(area);
            lv_display_flush_ready(display);
        });
    lv_display_add_event_cb(
        display_,
        [](lv_event_t* event) {
            auto* avatar = static_cast<AvatarDisplay*>(lv_event_get_user_data(event));
            avatar->RefreshComplete();
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
    source_ = static_cast<uint16_t*>(
        heap_caps_aligned_alloc(64, kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!frame_ || !source_) {
        heap_caps_free(frame_);
        heap_caps_free(source_);
        frame_ = source_ = nullptr;
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
    avatar::Composite(source_, rect, Pixels(offset));
}

void AvatarDisplay::LoadMode(int selected, int64_t ms) {
    const auto& mode = avatar::modes[selected];
    std::memcpy(frame_, Pixels(mode.base), kFrameBytes);
    std::memcpy(source_, frame_, kFrameBytes);
    current_mode_ = selected;
    eye_ = mouth_ = 0;
    blink_at_ = NextBlink(ms, mode);
    mouth_at_ = due_ms_ = last_pose_ms_ = ms;
    motion_ms_ = pose_start_us_ = stats_start_us_ = 0;
    pose_ = {};
    completed_ = transfer_bytes_ = frame_cost_ms_ = 0;
}

bool AvatarDisplay::Render(avatar::Rect area) {
    auto dirty = avatar::RenderRegion(source_, frame_, current_mode_, pose_, area);
    return dirty.w > 0 && dirty.h > 0 && Invalidate(dirty);
}

void AvatarDisplay::RefreshComplete() {
    if (!pose_start_us_)
        return;
    const auto now = esp_timer_get_time();
    const int cost = (now - pose_start_us_ + 999) / 1000;
    frame_cost_ms_ = std::max(cost, (frame_cost_ms_ * 7 + cost) / 8);
    pose_start_us_ = 0;
    ++completed_;
    const auto window = now - stats_start_us_;
    if (window >= 5000000) {
        ESP_LOGI("Avatar", "%s: %.1f completed poses/s, %.0f RGB666 bytes/s, recent frame %d ms",
                 current_mode_ ? "half" : "full", completed_ * 1e6 / window,
                 transfer_bytes_ * 1e6 / window, frame_cost_ms_);
        stats_start_us_ = now;
        completed_ = transfer_bytes_ = 0;
    }
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
            stats_start_us_ = pose_start_us_ = 0;
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
    const bool eye_changed = eye != eye_;
    bool mouth_changed = false;
    if (eye_changed) {
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
            mouth_changed = true;
            Patch(mode.mouth, mode.mouth_offsets[mouth]);
            mouth_ = mouth;
        }
        mouth_at_ = ms + mode.speech_ms;
    }
    if (!stats_start_us_) {
        stats_start_us_ = now_us;
        completed_ = transfer_bytes_ = 0;
    }
    if (eye_changed || mouth_changed) {
        if (eye_changed)
            Render(avatar::FaceBounds(mode.eyes));
        if (mouth_changed)
            Render(avatar::FaceBounds(mode.mouth));
        return;
    }
    if (ms < due_ms_ || pose_start_us_ || ms + frame_cost_ms_ >= blink_at_ ||
        (speaking && frame_cost_ms_ >= mode.speech_ms))
        return;
    const int interval = std::max(1000 / CONFIG_ANIME_TARGET_FPS, frame_cost_ms_ + 5);
    motion_ms_ += std::clamp<int64_t>(ms - last_pose_ms_, 0, interval);
    last_pose_ms_ = ms;
    due_ms_ = ms + interval;
    const avatar::MotionConfig cfg{CONFIG_ANIME_BREATH_PERIOD_MS,
                                   CONFIG_ANIME_BREATH_AMPLITUDE_PX,
                                   CONFIG_ANIME_ARM_PERIOD_MS,
                                   CONFIG_ANIME_RIGHT_ARM_PERIOD_MS,
                                   selected ? CONFIG_ANIME_ARM_HALF_PX : CONFIG_ANIME_ARM_FULL_PX,
                                   CONFIG_ANIME_HEAD_PERIOD_MS,
                                   CONFIG_ANIME_HEAD_TILT};
    pose_ = avatar::MakePose(motion_ms_, selected, cfg);
    bool changed = false;
    avatar::PoseRegions(selected, [&](avatar::Rect area) { changed |= Render(area); });
    if (changed)
        pose_start_us_ = now_us;
}
