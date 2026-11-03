#pragma once

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <array>
#include <cstdint>
#include <expected>
#include <mutex>
#include <string>

// 24BYJ48 + ULN2003，两轴各自排队，由同一固定任务输出八拍相序。
class MossMotor {
public:
    enum class Axis { Pitch = 0, Yaw = 1 };
    static constexpr size_t kQueueCapacity = 4;

    struct Config {
        std::array<std::array<gpio_num_t, 4>, 2> pins;
        int half_steps_per_revolution;
        int step_interval_ms;
        int max_relative_angle;
    };

    struct AxisStatus {
        bool moving = false;
        bool energized = false;
        size_t queued_commands = 0;
        uint32_t remaining_half_steps = 0;
        int64_t executed_half_steps = 0;  // 已输出的相序计数，不是位置反馈。
        esp_err_t last_error = ESP_OK;
    };

    struct Status {
        bool ready = false;
        std::array<AxisStatus, 2> axes;
    };

    MossMotor() = default;
    ~MossMotor();
    MossMotor(const MossMotor&) = delete;
    MossMotor& operator=(const MossMotor&) = delete;

    std::expected<void, std::string> Initialize(const Config& config);
    std::expected<void, std::string> MoveRelative(Axis axis, int angle);
    std::expected<void, std::string> Stop(Axis axis);
    Status GetStatus();

private:
    struct Command {
        uint32_t half_steps = 0;
        int direction = 1;
    };
    struct AxisState {
        std::array<Command, kQueueCapacity> queue;
        size_t head = 0;
        size_t count = 0;
        uint32_t remaining = 0;
        int direction = 1;
        int phase = -1;
        bool energized = false;
        int64_t executed = 0;
        esp_err_t error = ESP_OK;
    };

    std::mutex mutex_;
    Config config_{};
    std::array<AxisState, 2> axes_;
    TaskHandle_t worker_ = nullptr;
    SemaphoreHandle_t stopped_ = nullptr;
    bool ready_ = false;
    bool shutting_down_ = false;

    static void WorkerEntry(void* context);
    void Run();
    void Tick();
    esp_err_t WritePhase(size_t axis, uint8_t phase);
    esp_err_t StopLocked(size_t axis);
};

// 仅由 MossBoard 在电机初始化成功后调用，不依赖全局自动注册。
void RegisterMossMotorTools(MossMotor& motor);
