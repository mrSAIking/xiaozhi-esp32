#include "avatar_display.h"

#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#include <esp_random.h>
#include <esp_system.h>
#include <cstring>

#include "application.h"
#include "assets/lang_config.h"
#include "aura_service.h"
#include "companion_index.h"
#include "lvgl_font.h"

extern "C" const uint8_t boot_pixels[] asm("_binary_aura_boot_rgb565_start");
extern "C" const uint16_t companion_pixels[] asm("_binary_companion_pixels_bin_start");

namespace {
constexpr int kWidth = Ili9488::kWidth, kHeight = Ili9488::kHeight;
constexpr size_t kFrameBytes = kWidth * kHeight * sizeof(uint16_t);

constexpr int kPreviewMs = 5000;

int64_t NextBlink(int64_t ms, const avatar::Mode& mode) {
    return ms + mode.interval[0] + esp_random() % (mode.interval[1] - mode.interval[0] + 1);
}
}  // namespace

AvatarDisplay::AvatarDisplay() : LcdDisplay(nullptr, nullptr, kWidth, kHeight) {
    ESP_LOGI("Avatar", "Previous reset: %s", Board::ResetReasonName(esp_reset_reason()));
#if CONFIG_ANIME_PANEL_SELF_TEST
    panel_driver_.RunColorTest();
#endif
    const lv_area_t entire{0, 0, kWidth - 1, kHeight - 1};
    panel_driver_.Flush(entire, boot_pixels, kWidth * 2, kFrameBytes);
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

const uint16_t* AvatarDisplay::Pixels(int offset) const {
    return companion_pixels + (offset + (white_ ? avatar::background_stride : 0)) / 2;
}

void AvatarDisplay::SystemMessage(const char* title, const char* message) {
    DisplayLockGuard guard(this);
    if (!system_title_)
        return;
    lv_label_set_text(system_title_, title);
    lv_label_set_text(system_message_, message);
    if (lv_screen_active() != setup_screen_)
        lv_screen_load(setup_screen_);
}

void AvatarDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard guard(this);
    if (system_message_ && (activation_prompt_ || recovery_))
        lv_label_set_text(system_message_, content ? content : "");
    LcdDisplay::SetChatMessage(role, content);
}

void AvatarDisplay::ShowNotification(const char* text, int duration_ms) {
    DisplayLockGuard guard(this);
    if (!notice_) {
        LcdDisplay::ShowNotification(text, duration_ms);
        return;
    }
    lv_label_set_text(notice_, text);
    lv_obj_remove_flag(notice_, LV_OBJ_FLAG_HIDDEN);
    notice_until_ = esp_timer_get_time() / 1000 + duration_ms;
}

void AvatarDisplay::Volume(int percent) {
    DisplayLockGuard guard(this);
    if (!volume_box_)
        return;
    lv_label_set_text_fmt(volume_label_, LV_SYMBOL_VOLUME_MAX "   %d%%", percent);
    lv_bar_set_value(volume_bar_, percent, LV_ANIM_OFF);
    lv_obj_remove_flag(volume_box_, LV_OBJ_FLAG_HIDDEN);
    volume_until_ = esp_timer_get_time() / 1000 + 1500;
}

void AvatarDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard guard(this);
    LcdDisplay::SetTheme(theme);
    // SetTextFont retains the old owner only through this virtual call.
    // Rebind custom LVGL styles before that owner can be released.
    if (theme && system_message_) {
        auto font = theme->GetTextFont();
        lv_obj_set_style_text_font(system_message_, font ? font->font() : LV_FONT_DEFAULT, 0);
        lv_obj_set_style_text_font(notice_, font ? font->font() : LV_FONT_DEFAULT, 0);
    }
}

void AvatarDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    recovery_ = status && std::strcmp(status, Lang::Strings::ERROR) == 0;
    activation_prompt_ = status && std::strcmp(status, Lang::Strings::ACTIVATION) == 0;
    if (system_title_ && (activation_prompt_ || recovery_))
        lv_label_set_text(system_title_, activation_prompt_ ? "Activation" : "Recovery");
    LcdDisplay::SetStatus(status);
}
void AvatarDisplay::ShowProvisioning() {
    DisplayLockGuard lock(this);
    if (setup_screen_ && lv_screen_active() != setup_screen_)
        lv_screen_load(setup_screen_);
}
void AvatarDisplay::ConnectingWifi() {
    DisplayLockGuard guard(this);
    if (boot_status_)
        lv_label_set_text(boot_status_, "Connecting to Wi-Fi");
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
    ESP_LOGI("Avatar", "Creating Aura system screens");
    LcdDisplay::SetupUI();
    lv_timer_resume(lv_display_get_refr_timer(display_));
    descriptor_.header.magic = LV_IMAGE_HEADER_MAGIC;
    descriptor_.header.cf = LV_COLOR_FORMAT_RGB565;
    descriptor_.header.w = kWidth;
    descriptor_.header.h = kHeight;
    descriptor_.header.stride = kWidth * 2;
    descriptor_.data_size = kFrameBytes;
    boot_screen_ = lv_obj_create(nullptr);
    lv_obj_remove_style_all(boot_screen_);
    boot_image_ = descriptor_;
    boot_image_.data = boot_pixels;
    auto* logo = lv_image_create(boot_screen_);
    lv_image_set_src(logo, &boot_image_);
    auto* cover = lv_obj_create(boot_screen_);
    lv_obj_remove_style_all(cover);
    lv_obj_set_pos(cover, 0, 302);
    lv_obj_set_size(cover, 320, 66);
    lv_obj_set_style_bg_color(cover, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cover, 255, 0);
    boot_status_ = lv_label_create(cover);
    lv_label_set_text(boot_status_, "Initializing");
    lv_obj_set_style_text_color(boot_status_, lv_color_hex(0xd8d4df), 0);
    lv_obj_align(boot_status_, LV_ALIGN_TOP_MID, 0, 8);
    boot_fill_ = lv_obj_create(cover);
    lv_obj_remove_style_all(boot_fill_);
    lv_obj_set_size(boot_fill_, 24, 3);
    lv_obj_set_style_bg_color(boot_fill_, lv_color_hex(0x9333ea), 0);
    lv_obj_set_style_bg_opa(boot_fill_, 255, 0);
    setup_screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(setup_screen_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(setup_screen_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(setup_screen_, LV_OBJ_FLAG_SCROLLABLE);
    // Crop the centered logo from the existing boot art without a second asset.
    auto* crop = lv_obj_create(setup_screen_);
    lv_obj_remove_style_all(crop);
    lv_obj_remove_flag(crop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(crop, 320, 150);
    auto* mark = lv_image_create(crop);
    lv_image_set_src(mark, &boot_image_);
    lv_obj_set_y(mark, -142);
    auto label = [](lv_obj_t* parent, int y, uint32_t color) {
        auto* obj = lv_label_create(parent);
        lv_obj_set_width(obj, 284);
        lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
        lv_label_set_text(obj, "");
        return obj;
    };
    system_title_ = label(setup_screen_, 172, 0xda91ff);
    system_message_ = label(setup_screen_, 225, 0xffffff);
    if (auto font = GetTheme() ? GetTheme()->GetTextFont() : nullptr)
        lv_obj_set_style_text_font(system_message_, font->font(), 0);
    volume_box_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(volume_box_, 136, 57);
    lv_obj_align(volume_box_, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_pad_all(volume_box_, 7, 0);
    lv_obj_set_style_bg_color(volume_box_, lv_color_hex(0x17131c), 0);
    lv_obj_set_style_bg_opa(volume_box_, LV_OPA_90, 0);
    lv_obj_remove_flag(volume_box_, LV_OBJ_FLAG_SCROLLABLE);
    volume_label_ = lv_label_create(volume_box_);
    lv_obj_set_style_text_color(volume_label_, lv_color_white(), 0);
    volume_bar_ = lv_bar_create(volume_box_);
    lv_obj_set_size(volume_bar_, 112, 5);
    lv_obj_align(volume_bar_, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(volume_bar_, lv_color_hex(0x9333ea), LV_PART_INDICATOR);
    lv_obj_add_flag(volume_box_, LV_OBJ_FLAG_HIDDEN);
    notice_ = label(lv_layer_top(), 76, 0xffffff);
    lv_obj_set_style_bg_color(notice_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(notice_, LV_OPA_80, 0);
    lv_obj_add_flag(notice_, LV_OBJ_FLAG_HIDDEN);
#if !CONFIG_ANIME_SETUP_UI_ONLY
    frame_ = static_cast<uint16_t*>(
        heap_caps_aligned_alloc(64, kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (frame_) {
        avatar_screen_ = lv_obj_create(nullptr);
        lv_obj_remove_style_all(avatar_screen_);
        lv_obj_remove_flag(avatar_screen_, LV_OBJ_FLAG_SCROLLABLE);
        image_ = lv_image_create(avatar_screen_);
        descriptor_.data = reinterpret_cast<const uint8_t*>(frame_);
        LoadMode(requested_mode_.load(), esp_timer_get_time() / 1000);
        lv_image_set_src(image_, &descriptor_);
        lv_obj_set_pos(image_, 0, 0);
        CreateOverlays();
    } else {
        ESP_LOGE("Avatar", "Framebuffer allocation failed; keeping system UI available");
    }
#endif
    lv_screen_load(boot_screen_);
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
    white_ = requested_white_.load();
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
void AvatarDisplay::FinishConversationText() {
    std::lock_guard lock(captions_mutex_);
    captions_.Finish();
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
    lv_obj_set_size(caption_bar_, 320, 42);
    lv_obj_align(caption_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(caption_bar_, lv_color_hex(0x101012), 0);
    lv_obj_set_style_bg_opa(caption_bar_, 250, 0);
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
    int x;
    bool visible, font_changed = false;
    auto font = GetTheme() ? GetTheme()->GetTextFont() : nullptr;
    if (font && font != caption_font_) {
        lv_obj_set_style_text_font(caption_label_, font->font(), 0);
        caption_font_ = font;
        font_changed = true;
    }
    {
        std::lock_guard lock(captions_mutex_);
        if (font_changed)
            captions_.FontChanged();
        captions_.Tick(ms, active, [&](const char* str, size_t bytes) {
            return lv_text_get_width(str, bytes, lv_obj_get_style_text_font(caption_label_, 0),
                                     lv_obj_get_style_text_letter_space(caption_label_, 0));
        });
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
        }
        if (!visible)
            return;
        alpha = captions_.Opacity(ms);
        x = captions_.X();
    }
    if (alpha != opacity_) {
        lv_obj_set_style_opa(caption_bar_, alpha, 0);
        opacity_ = alpha;
    }

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
    if (volume_until_ && ms >= volume_until_) {
        lv_obj_add_flag(volume_box_, LV_OBJ_FLAG_HIDDEN);
        volume_until_ = 0;
    }
    if (notice_until_ && ms >= notice_until_) {
        lv_obj_add_flag(notice_, LV_OBJ_FLAG_HIDDEN);
        notice_until_ = 0;
    }
    const bool can_preview = state == kDeviceStateStarting ||
                             state == kDeviceStateWifiConfiguring ||
                             state == kDeviceStateActivating;
    if (preview_requested_.exchange(false) && can_preview)
        preview_until_ = ms + kPreviewMs;
    if (!can_preview)
        preview_until_ = 0;
    if ((!show || recovery_) && ms >= preview_until_) {
        if ((state == kDeviceStateStarting || state == kDeviceStateActivating) && !recovery_ &&
            !activation_prompt_ && boot_screen_) {
            if (lv_screen_active() != boot_screen_)
                lv_screen_load(boot_screen_);
            if (aura::IsOnline() &&
                std::strcmp(lv_label_get_text(boot_status_), "Connecting to AI") != 0)
                lv_label_set_text(boot_status_, "Connecting to AI");
            int phase = (ms / 12) % 144;
            lv_obj_set_pos(boot_fill_, 112 + (phase <= 72 ? phase : 144 - phase), 43);
            return;
        }
        if (lv_screen_active() != setup_screen_) {
            stats_start_us_ = pose_start_us_ = 0;
            lv_screen_load(setup_screen_);
        }
        return;
    }
    if (!frame_) {
        if (lv_screen_active() != setup_screen_)
            SystemMessage("System status",
                          "Character display unavailable. Settings are accessible.");
        return;
    }
    if (lv_screen_active() == boot_screen_)
        ShowNotification("Ready", 750);
    const int selected = requested_mode_.load();
    const auto& mode = avatar::modes[selected];
    if (current_mode_ != selected || white_ != requested_white_.load()) {
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
    UpdateOverlays(ms, listening, !app.GetAudioService().IsPlaybackIdle());
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
