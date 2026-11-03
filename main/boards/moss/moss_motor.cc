#include "moss_motor.h"

#include <esp_log.h>

namespace {
constexpr char kTag[] = "MossMotor";
constexpr uint8_t kPhases[] = {0x08, 0x0c, 0x04, 0x06, 0x02, 0x03, 0x01, 0x09};

bool ValidAxis(MossMotor::Axis axis) {
    return axis == MossMotor::Axis::Pitch || axis == MossMotor::Axis::Yaw;
}
}  // namespace

MossMotor::~MossMotor() {
    TaskHandle_t task;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_ = true;
        ready_ = false;
        task = worker_;
    }
    if (task != nullptr) {
        xTaskNotifyGive(task);
        xSemaphoreTake(stopped_, portMAX_DELAY);
    }
    if (stopped_ != nullptr) {
        vSemaphoreDelete(stopped_);
    }
}

std::expected<void, std::string> MossMotor::Initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_ || worker_ != nullptr || shutting_down_) {
        return std::unexpected("电机模块已初始化或正在关闭");
    }
    if (config.half_steps_per_revolution <= 0 || config.half_steps_per_revolution > 65536 ||
        config.step_interval_ms <= 0 || pdMS_TO_TICKS(config.step_interval_ms) == 0 ||
        config.max_relative_angle <= 0 || config.max_relative_angle > 360) {
        return std::unexpected("无效的电机步数、步进间隔或角度范围");
    }
    uint64_t mask = 0;
    for (const auto& pins : config.pins) {
        for (auto pin : pins) {
            if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || (mask & (1ULL << pin)) != 0) {
                return std::unexpected("电机引脚无效或重复");
            }
            mask |= 1ULL << pin;
        }
    }
    // 在切换为输出前先将输出锁存值置零，启动不自动运动。
    for (const auto& pins : config.pins) {
        for (auto pin : pins) {
            const auto result = gpio_set_level(pin, 0);
            if (result != ESP_OK) {
                return std::unexpected(std::string("电机 GPIO 初始化失败: ") +
                                       esp_err_to_name(result));
            }
        }
    }
    gpio_config_t io{};
    io.pin_bit_mask = mask;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    auto result = gpio_config(&io);
    if (result != ESP_OK) {
        return std::unexpected(std::string("电机 GPIO 配置失败: ") + esp_err_to_name(result));
    }
    config_ = config;
    if (stopped_ == nullptr) {
        stopped_ = xSemaphoreCreateBinary();
    }
    if (stopped_ == nullptr) {
        return std::unexpected("无法创建电机任务同步信号");
    }
    if (xTaskCreate(WorkerEntry, "MossMotor", 3072, this, 1, &worker_) != pdPASS) {
        worker_ = nullptr;
        return std::unexpected("无法创建电机运动任务");
    }
    ready_ = true;
    ESP_LOGI(kTag, "Two axes ready, %d half-steps/rev, %d ms/step",
             config_.half_steps_per_revolution, config_.step_interval_ms);
    return {};
}

std::expected<void, std::string> MossMotor::MoveRelative(Axis axis, int angle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_) {
        return std::unexpected("电机模块未就绪");
    }
    if (!ValidAxis(axis)) {
        return std::unexpected("未知电机轴");
    }
    if (angle < -config_.max_relative_angle || angle > config_.max_relative_angle) {
        return std::unexpected("相对角度超出单次命令范围");
    }
    auto& state = axes_[static_cast<size_t>(axis)];
    if (state.error != ESP_OK) {
        return std::unexpected("电机 GPIO 出错，请先停止该轴并检查状态");
    }
    if (angle == 0) {
        return {};
    }
    if (state.count == kQueueCapacity) {
        return std::unexpected("该轴运动队列已满，请稍后重试或停止");
    }
    const int magnitude = angle < 0 ? -angle : angle;
    const auto steps = static_cast<uint32_t>(
        (static_cast<int64_t>(magnitude) * config_.half_steps_per_revolution + 180) / 360);
    if (steps == 0) {
        return {};
    }
    state.queue[(state.head + state.count) % kQueueCapacity] = {steps, angle > 0 ? 1 : -1};
    ++state.count;
    return {};
}

esp_err_t MossMotor::WritePhase(size_t axis, uint8_t phase) {
    esp_err_t first_error = ESP_OK;
    for (size_t pin = 0; pin < 4; ++pin) {
        const auto result = gpio_set_level(config_.pins[axis][pin], (phase >> (3 - pin)) & 1);
        if (first_error == ESP_OK && result != ESP_OK) {
            first_error = result;
        }
    }
    return first_error;
}

esp_err_t MossMotor::StopLocked(size_t axis) {
    auto& state = axes_[axis];
    state.head = state.count = state.remaining = 0;
    state.error = WritePhase(axis, 0);
    state.energized = state.error != ESP_OK;
    return state.error;
}

std::expected<void, std::string> MossMotor::Stop(Axis axis) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || !ValidAxis(axis)) {
        return std::unexpected("电机未就绪或轴无效");
    }
    const auto result = StopLocked(static_cast<size_t>(axis));
    if (result != ESP_OK) {
        return std::unexpected(std::string("电机关闭输出失败: ") + esp_err_to_name(result));
    }
    return {};
}

MossMotor::Status MossMotor::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    Status status;
    status.ready = ready_;
    for (size_t i = 0; i < axes_.size(); ++i) {
        const auto& state = axes_[i];
        status.axes[i] = {state.remaining != 0 || state.count != 0 || state.energized,
                          state.energized,
                          state.count,
                          state.remaining,
                          state.executed,
                          state.error};
    }
    return status;
}

void MossMotor::Tick() {
    // 与提交命令、停止及状态快照共用同一把锁，停止返回后旧命令不会重新输出。
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < axes_.size(); ++i) {
        auto& state = axes_[i];
        if (state.error != ESP_OK) {
            continue;
        }
        if (state.remaining == 0 && state.count != 0) {
            const auto command = state.queue[state.head];
            state.head = (state.head + 1) % kQueueCapacity;
            --state.count;
            state.remaining = command.half_steps;
            state.direction = command.direction;
        }
        if (state.remaining == 0) {
            // 最后一拍已保持一个完整间隔后再关闭输出，空闲时不持续励磁。
            if (state.energized) {
                state.error = WritePhase(i, 0);
                state.energized = state.error != ESP_OK;
            }
            continue;
        }
        state.phase = state.phase < 0 ? (state.direction > 0 ? 0 : 7)
                                      : (state.phase + state.direction + 8) % 8;
        const auto result = WritePhase(i, kPhases[state.phase]);
        if (result != ESP_OK) {
            StopLocked(i);
            state.error = result;
            ESP_LOGE(kTag, "Axis %u GPIO failed: %s", static_cast<unsigned>(i),
                     esp_err_to_name(result));
            continue;
        }
        state.energized = true;
        --state.remaining;
        state.executed += state.direction;
    }
}

void MossMotor::WorkerEntry(void* context) { static_cast<MossMotor*>(context)->Run(); }

void MossMotor::Run() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutting_down_) {
                StopLocked(0);
                StopLocked(1);
                break;
            }
        }
        Tick();
        // 只有关闭模块时通知此任务；新命令不会打断相位保持时间。
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(config_.step_interval_ms));
    }
    xSemaphoreGive(stopped_);
    vTaskDelete(nullptr);
}
