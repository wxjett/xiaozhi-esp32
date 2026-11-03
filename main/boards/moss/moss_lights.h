#pragma once

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <array>
#include <expected>
#include <mutex>
#include <string>

// MOSS 五路流水灯和单路 PWM 眼灯；由一个固定任务更新灯效。
class MossLights {
public:
    enum class EyeMode { Off, On, Breathing, Paused };
    struct Config {
        std::array<gpio_num_t, 5> bar_pins;
        gpio_num_t eye_pin;
    };
    struct Status {
        bool ready = false;
        bool flowing = false;
        unsigned bar_pattern = 0;
        EyeMode eye_mode = EyeMode::Off;
        unsigned eye_duty = 0;
        esp_err_t bar_error = ESP_OK;
        esp_err_t eye_error = ESP_OK;
    };

    MossLights() = default;
    ~MossLights();
    MossLights(const MossLights&) = delete;
    MossLights& operator=(const MossLights&) = delete;

    std::expected<void, std::string> Initialize(const Config& config);
    std::expected<void, std::string> SetFlowing(bool enabled);
    std::expected<void, std::string> SetEyeMode(EyeMode mode);
    std::expected<void, std::string> ResumeBreathing();
    Status GetStatus();

private:
    std::mutex mutex_;
    Config config_{};
    Status status_;
    TaskHandle_t worker_ = nullptr;
    SemaphoreHandle_t stopped_ = nullptr;
    bool bar_initialized_ = false;
    bool eye_initialized_ = false;
    bool shutting_down_ = false;
    size_t flow_stage_ = 0;
    unsigned flow_elapsed_ms_ = 0;
    unsigned eye_elapsed_ms_ = 0;
    int eye_direction_ = 1;

    esp_err_t WriteBar(unsigned pattern);
    esp_err_t WriteEye(unsigned duty);
    void Tick();
    static void WorkerEntry(void* context);
    void Run();
};

void RegisterMossLightTools(MossLights& lights);
