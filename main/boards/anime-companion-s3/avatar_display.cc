#include "avatar_display.h"

#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#include <esp_random.h>
#include <esp_system.h>
#include <cstring>

#include "application.h"
#include "companion_index.h"
#include "lvgl_font.h"

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
    CreateOverlays();
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
    avatar::Composite(frame_, rect, Pixels(offset));
    Invalidate(rect);
}

void AvatarDisplay::LoadMode(int selected, int64_t ms) {
    const auto& mode = avatar::modes[selected];
    std::memcpy(frame_, Pixels(mode.base), kFrameBytes);

    current_mode_ = selected;
    eye_ = mouth_ = 0;
    blink_at_ = NextBlink(ms, mode);
    mouth_at_ = due_ms_ = origin_ms_ = ms;
    region_ = 0;
    pose_start_us_ = stats_start_us_ = 0;
    pose_ = {};
    completed_ = transfer_bytes_ = frame_cost_ms_ = 0;
}

int AvatarDisplay::Render(avatar::Rect area) {
    auto dirty = avatar::RenderRegion(Pixels(avatar::modes[current_mode_].base), frame_,
                                      current_mode_, pose_, area);
    return dirty.w > 0 && dirty.h > 0 && Invalidate(dirty) ? dirty.w * dirty.h : 0;
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

void AvatarDisplay::SetTextMode(bool enabled) {
    std::lock_guard lock(captions_mutex_);
    captions_.Enable(enabled);
}

void AvatarDisplay::SetConversationText(const char* role, const char* content) {
    if (!frame_) {
        LcdDisplay::SetChatMessage(role, content);
        return;
    }
    const bool answer = std::strcmp(role, "assistant") == 0;
    if (answer && Application::GetInstance().IsSpeechAborted())
        return;
    std::lock_guard lock(captions_mutex_);
    captions_.Submit(answer, content, esp_timer_get_time() / 1000);
}
void AvatarDisplay::CancelConversationText() {
    std::lock_guard lock(captions_mutex_);
    captions_.Clear();
}

void AvatarDisplay::CreateOverlays() {
    dot_ = lv_obj_create(avatar_screen_);
    lv_obj_remove_style_all(dot_);
    lv_obj_set_size(dot_, 7, 7);
    lv_obj_align(dot_, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_radius(dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot_, lv_color_hex(0x22ff66), 0);
    lv_obj_set_style_bg_opa(dot_, LV_OPA_COVER, 0);
    lv_obj_add_flag(dot_, LV_OBJ_FLAG_HIDDEN);

    caption_bar_ = lv_obj_create(avatar_screen_);
    lv_obj_remove_style_all(caption_bar_);
    lv_obj_remove_flag(caption_bar_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(caption_bar_, 320, 58);
    lv_obj_align(caption_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(caption_bar_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(caption_bar_, LV_OPA_80, 0);
    lv_obj_add_flag(caption_bar_, LV_OBJ_FLAG_HIDDEN);
    caption_label_ = lv_label_create(caption_bar_);
    lv_label_set_text(caption_label_, "");
    lv_obj_set_width(caption_label_, LV_SIZE_CONTENT);
    lv_obj_align(caption_label_, LV_ALIGN_LEFT_MID, 0, 0);
}

void AvatarDisplay::UpdateOverlays(int64_t ms, bool listening, bool active) {
    if (listening_ != listening) {
        listening_ = listening;
        if (listening)
            lv_obj_remove_flag(dot_, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(dot_, LV_OBJ_FLAG_HIDDEN);
    }
    int alpha;
    int64_t elapsed;
    bool visible, font_changed = false;
    auto font = GetTheme() ? GetTheme()->GetTextFont() : nullptr;
    if (font && font != caption_font_) {
        lv_obj_set_style_text_font(caption_label_, font->font(), 0);
        caption_font_ = font;
        font_changed = true;
    }
    {
        std::lock_guard lock(captions_mutex_);
        captions_.Tick(ms, active);
        visible = !captions_.text.empty();
        if (caption_revision_ != captions_.revision || font_changed) {
            caption_revision_ = captions_.revision;
            if (!visible) {
                lv_obj_add_flag(caption_bar_, LV_OBJ_FLAG_HIDDEN);
                return;
            }
            lv_obj_remove_flag(caption_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(caption_label_, captions_.text.c_str());
            lv_obj_set_style_text_color(caption_label_,
                                        lv_color_hex(captions_.assistant ? 0xffffff : 0xa8c7fa), 0);
            lv_obj_update_layout(caption_label_);
            caption_width_ = lv_obj_get_width(caption_label_);
        }
        if (!visible)
            return;
        alpha = captions_.Opacity(ms);
        elapsed = ms - captions_.shown;
    }
    if (alpha != opacity_) {
        lv_obj_set_style_opa(caption_bar_, alpha, 0);
        opacity_ = alpha;
    }
    const int x = 8 + aura::ScrollX(caption_width_, elapsed);
    if (x != caption_x_) {
        lv_obj_set_x(caption_label_, x);
        caption_x_ = x;
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
            Patch(mode.mouth, mode.mouth_offsets[mouth]);
            mouth_ = mouth;
        }
        mouth_at_ = ms + mode.speech_ms;
    }
    if (!stats_start_us_) {
        stats_start_us_ = now_us;
        completed_ = transfer_bytes_ = 0;
    }
    const bool listening = aura::Listening(
        state == kDeviceStateListening,
        state == kDeviceStateListening && app.GetAudioService().IsAudioProcessorRunning(),
        app.IsListeningStartPending());
    UpdateOverlays(ms, listening, speaking || !app.GetAudioService().IsPlaybackIdle());
    if (ms < due_ms_ || pose_start_us_)
        return;
    if (!region_) {
        const avatar::MotionConfig cfg{
            CONFIG_ANIME_BREATH_PERIOD_MS, CONFIG_ANIME_BREATH_AMPLITUDE_PX,
            CONFIG_ANIME_ARM_PERIOD_MS, CONFIG_ANIME_RIGHT_ARM_PERIOD_MS,
            selected ? CONFIG_ANIME_ARM_HALF_PX : CONFIG_ANIME_ARM_FULL_PX};
        pose_ = avatar::MakePose(ms - origin_ms_, cfg);
        sweep_start_us_ = now_us;
    }
    aura::BodyBudget budget(CONFIG_ANIME_SPI_MHZ, speaking);
    int changed = 0;
    while (budget.Available() && changed < 8) {
        const auto band = avatar::BodyRegion(selected, region_);
        if (!band.h) {
            region_ = 0;
            due_ms_ = sweep_start_us_ / 1000 + 1000 / CONFIG_ANIME_TARGET_FPS;
            if (changed)
                pose_start_us_ = sweep_start_us_;
            else {
                pose_start_us_ = sweep_start_us_;
                RefreshComplete();
            }
            break;
        }
        const auto before = esp_timer_get_time();
        const int pixels = Render(band);
        changed += pixels != 0;
        budget.Charge(pixels, esp_timer_get_time() - before);
        ++region_;
    }
}
