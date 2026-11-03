#include "moss_lights.h"

#include <driver/ledc.h>
#include <esp_log.h>

namespace {
constexpr char kTag[] = "MossLights";
constexpr unsigned kTickMs = 10;
constexpr unsigned kMaxDuty = 8191;
// 位4～位0分别对应五个 GPIO，保留旧 MOSS 的图案和各阶段时长。
constexpr unsigned kPatterns[] = {0b10001, 0b01010, 0b01100, 0b10110, 0b00101};
constexpr unsigned kDurations[] = {110, 100, 90, 100, 130};
// 避开公共背光类使用的 timer 0 / channel 0。
constexpr auto kPwmMode = LEDC_LOW_SPEED_MODE;
constexpr auto kPwmTimer = LEDC_TIMER_1;
constexpr auto kPwmChannel = LEDC_CHANNEL_1;

std::expected<void, std::string> CheckOutput(esp_err_t error) {
    if (error != ESP_OK) {
        return std::unexpected(std::string("灯光输出失败: ") + esp_err_to_name(error));
    }
    return {};
}
}  // namespace

MossLights::~MossLights() {
    TaskHandle_t task;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_ = true;
        status_.ready = false;
        task = worker_;
    }
    if (task != nullptr) {
        xTaskNotifyGive(task);
        xSemaphoreTake(stopped_, portMAX_DELAY);
    }
    if (bar_initialized_) {
        WriteBar(0);
    }
    if (eye_initialized_) {
        ledc_stop(kPwmMode, kPwmChannel, 0);
    }
    if (stopped_ != nullptr) {
        vSemaphoreDelete(stopped_);
    }
}

std::expected<void, std::string> MossLights::Initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bar_initialized_ || eye_initialized_ || shutting_down_) {
        return std::unexpected("灯光模块已初始化或正在关闭");
    }
    uint64_t mask = 0;
    for (auto pin : config.bar_pins) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || (mask & (1ULL << pin)) != 0) {
            return std::unexpected("流水灯引脚无效或重复");
        }
        mask |= 1ULL << pin;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(config.eye_pin) || (mask & (1ULL << config.eye_pin)) != 0) {
        return std::unexpected("眼灯引脚无效或与流水灯重复");
    }
    if (pdMS_TO_TICKS(kTickMs) == 0) {
        return std::unexpected("系统时钟无法支持灯效更新间隔");
    }
    config_ = config;
    auto result = WriteBar(0);
    if (result != ESP_OK) {
        return CheckOutput(result);
    }
    gpio_config_t io{};
    io.pin_bit_mask = mask;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    result = gpio_config(&io);
    if (result != ESP_OK) {
        return CheckOutput(result);
    }
    bar_initialized_ = true;

    ledc_timer_config_t timer{};
    timer.speed_mode = kPwmMode;
    timer.duty_resolution = LEDC_TIMER_13_BIT;
    timer.timer_num = kPwmTimer;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    result = ledc_timer_config(&timer);
    if (result != ESP_OK) {
        return CheckOutput(result);
    }
    ledc_channel_config_t channel{};
    channel.gpio_num = config.eye_pin;
    channel.speed_mode = kPwmMode;
    channel.channel = kPwmChannel;
    channel.timer_sel = kPwmTimer;
    channel.duty = 0;
    result = ledc_channel_config(&channel);
    if (result != ESP_OK) {
        return CheckOutput(result);
    }
    eye_initialized_ = true;
    stopped_ = xSemaphoreCreateBinary();
    if (stopped_ == nullptr) {
        return std::unexpected("无法创建灯光任务同步信号");
    }
    if (xTaskCreate(WorkerEntry, "MossLights", 3072, this, 1, &worker_) != pdPASS) {
        worker_ = nullptr;
        return std::unexpected("无法创建灯光任务");
    }
    status_.ready = true;
    ESP_LOGI(kTag, "Five GPIO lamps and PWM eye ready, initially off");
    return {};
}

esp_err_t MossLights::WriteBar(unsigned pattern) {
    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < config_.bar_pins.size(); ++i) {
        const auto result = gpio_set_level(config_.bar_pins[i], (pattern >> (4 - i)) & 1);
        if (first_error == ESP_OK && result != ESP_OK) {
            first_error = result;
        }
    }
    if (first_error == ESP_OK) {
        status_.bar_pattern = pattern;
    }
    return first_error;
}

esp_err_t MossLights::WriteEye(unsigned duty) {
    auto result = ledc_set_duty(kPwmMode, kPwmChannel, duty);
    if (result == ESP_OK) {
        result = ledc_update_duty(kPwmMode, kPwmChannel);
    }
    if (result == ESP_OK) {
        status_.eye_duty = duty;
    }
    return result;
}

std::expected<void, std::string> MossLights::SetFlowing(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.ready) {
        return std::unexpected("灯光模块未就绪");
    }
    if (enabled && status_.flowing) {
        return {};
    }
    status_.flowing = false;
    flow_stage_ = flow_elapsed_ms_ = 0;
    status_.bar_error = WriteBar(enabled ? kPatterns[0] : 0);
    if (status_.bar_error == ESP_OK) {
        status_.flowing = enabled;
    } else {
        WriteBar(0);  // 失败后尽力熄灭，但保留原始错误供查询。
    }
    return CheckOutput(status_.bar_error);
}

std::expected<void, std::string> MossLights::SetEyeMode(EyeMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.ready) {
        return std::unexpected("灯光模块未就绪");
    }
    if (mode == EyeMode::Paused) {
        if (status_.eye_mode != EyeMode::Breathing && status_.eye_mode != EyeMode::Paused) {
            return std::unexpected("眼灯未处于呼吸模式，无法暂停");
        }
        status_.eye_mode = mode;
        return {};
    }
    if (mode == EyeMode::Breathing &&
        (status_.eye_mode == EyeMode::Paused || status_.eye_mode == EyeMode::Breathing)) {
        status_.eye_mode = mode;  // 恢复时保留亮度和方向。
        return {};
    }
    if (mode != EyeMode::Off && mode != EyeMode::On && mode != EyeMode::Breathing) {
        return std::unexpected("未知眼灯模式");
    }
    status_.eye_mode = EyeMode::Off;  // 先取消旧动画，关闭返回后不会被重新点亮。
    eye_direction_ = 1;
    eye_elapsed_ms_ = 0;
    status_.eye_error = WriteEye(mode == EyeMode::On ? kMaxDuty : 0);
    if (status_.eye_error == ESP_OK) {
        status_.eye_mode = mode;
    } else {
        WriteEye(0);
    }
    return CheckOutput(status_.eye_error);
}

MossLights::Status MossLights::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::expected<void, std::string> MossLights::ResumeBreathing() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.ready || status_.eye_mode != EyeMode::Paused) {
        return std::unexpected("眼灯未就绪或未处于暂停状态，无法恢复");
    }
    status_.eye_mode = EyeMode::Breathing;
    return {};
}

void MossLights::Tick() {
    // 所有输出与控制命令共用一把锁；两个灯效互不改变对方的状态。
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.flowing) {
        flow_elapsed_ms_ += kTickMs;
        if (flow_elapsed_ms_ >= kDurations[flow_stage_]) {
            flow_elapsed_ms_ = 0;
            flow_stage_ = (flow_stage_ + 1) % 5;
            status_.bar_error = WriteBar(kPatterns[flow_stage_]);
            if (status_.bar_error != ESP_OK) {
                status_.flowing = false;
                WriteBar(0);
                ESP_LOGE(kTag, "Flow output failed: %s", esp_err_to_name(status_.bar_error));
            }
        }
    }
    if (status_.eye_mode == EyeMode::Breathing) {
        eye_elapsed_ms_ += kTickMs;
        if (eye_elapsed_ms_ >= 20) {
            eye_elapsed_ms_ = 0;
            int duty = static_cast<int>(status_.eye_duty) + eye_direction_ * 100;
            if (duty >= static_cast<int>(kMaxDuty)) {
                duty = kMaxDuty;
                eye_direction_ = -1;
            } else if (duty <= 0) {
                duty = 0;
                eye_direction_ = 1;
            }
            status_.eye_error = WriteEye(duty);
            if (status_.eye_error != ESP_OK) {
                status_.eye_mode = EyeMode::Off;
                WriteEye(0);
                ESP_LOGE(kTag, "Eye PWM failed: %s", esp_err_to_name(status_.eye_error));
            }
        }
    }
}

void MossLights::WorkerEntry(void* context) { static_cast<MossLights*>(context)->Run(); }

void MossLights::Run() {
    while (true) {
        // 仅析构时唤醒；控制命令不会增加动画更新速度。
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kTickMs));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutting_down_) {
                break;
            }
        }
        Tick();
    }
    xSemaphoreGive(stopped_);
    vTaskDelete(nullptr);
}
