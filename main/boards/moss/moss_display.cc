#include "moss_display.h"

#include "application.h"
#include "assets/boot_animation.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"

#include <esp_err.h>
#include <esp_lvgl_port.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#define TAG "MossDisplay"

static_assert(moss_assets::kBootAnimationDurationMs == 6000);

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_1);
LV_FONT_DECLARE(font_noto_emoji_30_1);

MossDisplay::MossDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_1);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_1);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    ESP_LOGI(TAG, "Adding OLED display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

MossDisplay::~MossDisplay() {
    {
        DisplayLockGuard lock(this);
        if (lock) {
            if (ui_timer_ != nullptr) {
                lv_timer_delete(ui_timer_);
                ui_timer_ = nullptr;
            }
            if (root_ != nullptr) {
                lv_obj_delete(root_);
                root_ = nullptr;
            }
        }
    }
    if (display_ != nullptr) {
        lvgl_port_remove_disp(display_);
        display_ = nullptr;
    }
    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    lvgl_port_deinit();
}

bool MossDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void MossDisplay::Unlock() { lvgl_port_unlock(); }

std::string MossDisplay::LimitText(const char* text) {
    if (text == nullptr) {
        return {};
    }
    std::string result(text);
    if (result.size() > kMaxMessageBytes) {
        size_t end = kMaxMessageBytes;
        while ((static_cast<unsigned char>(result[end]) & 0xc0) == 0x80) {
            --end;
        }
        result.resize(end);
        result += "...";
    }
    std::replace(result.begin(), result.end(), '\r', ' ');
    return result;
}

void MossDisplay::SetupUI() {
    DisplayLockGuard lock(this);
    if (!lock || setup_ui_called_ || display_ == nullptr) {
        return;
    }
    root_ = lv_obj_create(lv_display_get_screen_active(display_));
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, width_, height_);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    // esp_lvgl_port 的 RGB565 单色转换将黑像素映射为 OLED 亮点。
    lv_obj_set_style_bg_color(root_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(root_, lv_color_black(), 0);
    lv_obj_set_style_text_font(root_, current_theme_->GetTextFont()->font(), 0);
    lv_obj_set_style_text_line_space(root_, 0, 0);

    boot_image_ = lv_image_create(root_);
    static_assert(kBootDurationMs == moss_assets::kBootAnimationDurationMs);
    lv_image_set_src(boot_image_, &moss_assets::kBootFrames[0]);
    lv_obj_center(boot_image_);
    if (height_ < 64) {
        lv_image_set_scale(boot_image_, height_ * 256 / 64);
    }
    Display::SetupUI();
    lv_refr_now(display_);
    boot_started_ = lv_tick_get();
    activity_started_ = boot_started_;
    ui_timer_ = lv_timer_create(
        [](lv_timer_t* timer) { static_cast<MossDisplay*>(lv_timer_get_user_data(timer))->Tick(); },
        10, this);
    ESP_LOGI(TAG, "Boot animation started, duration=%lu ms",
             static_cast<unsigned long>(kBootDurationMs));
}

void MossDisplay::Tick() {
    // LVGL 定时器在 LVGL 任务中运行，已持有显示锁。
    if (sleeping_) {
        return;
    }
    if (showing_boot_) {
        const uint32_t elapsed = lv_tick_elaps(boot_started_);
        if (elapsed < kBootDurationMs) {
            // 按 GIF 原时序选择帧。刷新延迟时跳过过期帧，不累积播放时长误差。
            size_t next = boot_frame_index_;
            while (next + 1 < moss_assets::kBootFrameCount &&
                   elapsed >= moss_assets::kBootFrameEndsMs[next]) {
                ++next;
            }
            if (next != boot_frame_index_) {
                boot_frame_index_ = next;
                lv_image_set_src(boot_image_, &moss_assets::kBootFrames[next]);
            }
            return;
        }
        ShowConsole();
        lv_timer_set_period(ui_timer_, 200);
    }
    RefreshActivity();
}

void MossDisplay::ShowConsole() {
    lv_obj_delete(boot_image_);
    boot_image_ = nullptr;
    showing_boot_ = false;
    const int line_height = lv_font_get_line_height(current_theme_->GetTextFont()->font());
    const int footer_height = std::min(line_height, height_ / 2);
    log_view_ = lv_obj_create(root_);
    lv_obj_remove_style_all(log_view_);
    lv_obj_set_size(log_view_, width_, height_ - footer_height);
    lv_obj_set_scroll_dir(log_view_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(log_view_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(log_view_, LV_OBJ_FLAG_SCROLL_ELASTIC);
    log_label_ = lv_label_create(log_view_);
    lv_obj_set_width(log_label_, width_);
    lv_label_set_long_mode(log_label_, LV_LABEL_LONG_WRAP);
    activity_label_ = lv_label_create(root_);
    lv_obj_set_size(activity_label_, width_, footer_height);
    lv_obj_align(activity_label_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_long_mode(activity_label_, LV_LABEL_LONG_CLIP);
    if (entries_.empty()) {
        AppendLog("system", "MOSS READY");
    }
    RefreshLog();
    RefreshActivity();
    ESP_LOGI(TAG, "Boot animation finished, console shown");
}

void MossDisplay::AppendLog(const std::string& role, const std::string& text) {
    entries_.push_back({role, text});
    while (entries_.size() > kMaxEntries) {
        entries_.pop_front();
    }
    if (!showing_boot_) {
        RefreshLog();
    }
}

void MossDisplay::RefreshLog() {
    if (log_label_ == nullptr) {
        return;
    }
    std::string text;
    for (const auto& entry : entries_) {
        if (!text.empty()) {
            text += '\n';
        }
        text += entry.role == "user" ? "U: " : entry.role == "assistant" ? "M: " : "> ";
        text += entry.text;
    }
    lv_label_set_text(log_label_, text.c_str());
    lv_obj_update_layout(log_view_);
    lv_obj_scroll_to_y(log_view_,
                       lv_obj_get_scroll_bottom(log_view_) + lv_obj_get_scroll_y(log_view_),
                       LV_ANIM_ON);
}

void MossDisplay::RefreshActivity() {
    if (activity_label_ == nullptr) {
        return;
    }
    const auto state = Application::GetInstance().GetDeviceState();
    if (last_state_ != state) {
        last_state_ = state;
        activity_started_ = lv_tick_get();
        if (state == kDeviceStateIdle || state == kDeviceStateSpeaking ||
            state == kDeviceStateNotifying || state == kDeviceStateFatalError) {
            waiting_for_reply_ = false;
        }
    }
    if (!notification_.empty() && lv_tick_elaps(notification_started_) < notification_duration_) {
        lv_label_set_text(activity_label_, notification_.c_str());
        return;
    }
    notification_.clear();
    const char* activity = "READY";
    switch (state) {
        case kDeviceStateStarting:
            activity = "START";
            break;
        case kDeviceStateWifiConfiguring:
            activity = "WIFI SETUP";
            break;
        case kDeviceStateConnecting:
            activity = "CONNECT";
            break;
        case kDeviceStateListening:
            activity = "LISTEN";
            break;
        case kDeviceStateSpeaking:
            activity = "SPEAK";
            break;
        case kDeviceStateNotifying:
            activity = "PLAY";
            break;
        case kDeviceStateUpgrading:
            activity = "UPDATE";
            break;
        case kDeviceStateFatalError:
            activity = "ERROR";
            break;
        default:
            break;
    }
    if (waiting_for_reply_) {
        activity = "WAIT";
    }
    const uint32_t elapsed = lv_tick_elaps(activity_started_);
    char buffer[48];
    // 仅显示真实设备状态和等待计时，不生成虚构的推理日志。
    std::snprintf(buffer, sizeof(buffer), "> %s %lu.%lus %c", activity,
                  static_cast<unsigned long>(elapsed / 1000),
                  static_cast<unsigned long>((elapsed / 100) % 10),
                  (lv_tick_get() / 500) % 2 == 0 ? '_' : ' ');
    lv_label_set_text(activity_label_, buffer);
}

void MossDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    auto text = LimitText(status);
    if (!text.empty() && text != status_) {
        status_ = text;
        AppendLog("system", text);
    }
}

void MossDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    notification_ = LimitText(notification);
    notification_started_ = lv_tick_get();
    notification_duration_ = static_cast<uint32_t>(std::max(0, duration_ms));
    if (!notification_.empty()) {
        AppendLog("system", notification_);
    }
    RefreshActivity();
}

void MossDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!lock) {
        return;
    }
    auto text = LimitText(content);
    // 公共业务用空消息清空字幕；日志主界面保留此前记录。
    if (text.empty()) {
        return;
    }
    const std::string kind = role != nullptr && std::strcmp(role, "user") == 0        ? "user"
                             : role != nullptr && std::strcmp(role, "assistant") == 0 ? "assistant"
                                                                                      : "system";
    AppendLog(kind, text);
    if (kind == "user") {
        waiting_for_reply_ = true;
        activity_started_ = lv_tick_get();
    } else if (kind == "assistant") {
        waiting_for_reply_ = false;
    }
    RefreshActivity();
}

void MossDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!lock || role == nullptr || content == nullptr) {
        return;
    }
    const auto text = LimitText(content);
    auto it = std::find_if(entries_.rbegin(), entries_.rend(), [&](const LogEntry& entry) {
        return entry.role == role && entry.text == text;
    });
    if (it != entries_.rend()) {
        entries_.erase(std::next(it).base());
        RefreshLog();
    }
}

void MossDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (lock) {
        // 结束当前字幕/等待状态，但保留终端历史；容量由 kMaxEntries 限制。
        waiting_for_reply_ = false;
        RefreshActivity();
    }
}

bool MossDisplay::SetEmotion(const char* emotion) {
    // 本界面不渲染表情，保留通用 Display 接口。
    return false;
}

void MossDisplay::UpdateStatusBar(bool update_all) {
    DisplayLockGuard lock(this);
    if (lock) {
        RefreshActivity();
    }
}

void MossDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    if (!lock || theme == nullptr) {
        return;
    }
    Display::SetTheme(theme);
    if (root_ != nullptr && theme->GetTextFont() != nullptr) {
        lv_obj_set_style_text_font(root_, theme->GetTextFont()->font(), 0);
        RefreshLog();
    }
}

void MossDisplay::SetPowerSaveMode(bool on) {
    DisplayLockGuard lock(this);
    if (!lock || panel_ == nullptr) {
        return;
    }
    // 启动动画的 6 秒展示不被后台节电请求打断。
    if (showing_boot_ && on) {
        return;
    }
    sleeping_ = on;
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, !on));
}
