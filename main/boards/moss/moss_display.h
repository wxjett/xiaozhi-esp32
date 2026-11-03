#ifndef MOSS_DISPLAY_H
#define MOSS_DISPLAY_H

#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <deque>
#include <string>

// MOSS 专属 OLED：6 秒点阵启动动画、滚动日志和实时状态行。
class MossDisplay : public LvglDisplay {
public:
    // 接收已初始化的 OLED 句柄，并接管其生命周期。
    MossDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, bool mirror_x, bool mirror_y);
    ~MossDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    using LvglDisplay::ShowNotification;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void SetChatMessage(const char* role, const char* content) override;
    void RemoveChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    bool SetEmotion(const char* emotion) override;
    bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetTheme(Theme* theme) override;
    void SetPowerSaveMode(bool on) override;
    bool IsMonochrome() const override { return true; }

private:
    // 从首个启动画面提交刷新后计时，不阻塞网络和音频启动。
    static constexpr uint32_t kBootDurationMs = 6000;
    static constexpr int kHeaderHeight = 16;
    static constexpr int kLogLineHeight = 12;
    static constexpr size_t kMaxEntries = 16;
    static constexpr size_t kMaxMessageBytes = 512;
    struct LogEntry {
        std::string role;
        std::string text;
    };

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* boot_image_ = nullptr;
    lv_obj_t* log_view_ = nullptr;
    lv_obj_t* log_label_ = nullptr;
    lv_obj_t* activity_label_ = nullptr;
    lv_timer_t* ui_timer_ = nullptr;
    std::deque<LogEntry> entries_;
    std::string status_;
    std::string notification_;
    uint32_t boot_started_ = 0;
    size_t boot_frame_index_ = 0;
    uint32_t activity_started_ = 0;
    uint32_t notification_started_ = 0;
    uint32_t notification_duration_ = 0;
    int last_state_ = -1;
    bool showing_boot_ = true;
    bool waiting_for_reply_ = false;
    bool sleeping_ = false;

    static std::string LimitText(const char* text);
    void Tick();
    void ShowConsole();
    void AppendLog(const std::string& role, const std::string& text);
    void RefreshLog();
    void RefreshActivity();
    void RefreshNetworkIcon();

    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;
};

#endif  // MOSS_DISPLAY_H
