#include "moss_lights.h"

#include "mcp_server.h"

namespace {
const char* EyeModeName(MossLights::EyeMode mode) {
    switch (mode) {
        case MossLights::EyeMode::On:
            return "on";
        case MossLights::EyeMode::Breathing:
            return "breathing";
        case MossLights::EyeMode::Paused:
            return "paused";
        default:
            return "off";
    }
}

ToolResult LightStatus(MossLights& lights, bool eye) {
    const auto status = lights.GetStatus();
    CJsonUniquePtr root(cJSON_CreateObject());
    if (!root) {
        return std::unexpected("无法创建灯光状态响应");
    }
    cJSON_AddBoolToObject(root.get(), "ready", status.ready);
    if (eye) {
        cJSON_AddStringToObject(root.get(), "mode", EyeModeName(status.eye_mode));
        cJSON_AddNumberToObject(root.get(), "duty", status.eye_duty);
        cJSON_AddNumberToObject(root.get(), "max_duty", 8191);
        cJSON_AddStringToObject(root.get(), "last_error", esp_err_to_name(status.eye_error));
    } else {
        cJSON_AddBoolToObject(root.get(), "flowing", status.flowing);
        cJSON_AddNumberToObject(root.get(), "led_count", 5);
        cJSON_AddNumberToObject(root.get(), "pattern", status.bar_pattern);
        cJSON_AddStringToObject(root.get(), "last_error", esp_err_to_name(status.bar_error));
    }
    return root.release();
}
}  // namespace

void RegisterMossLightTools(MossLights& lights) {
    auto& server = McpServer::GetInstance();
    server.AddTool(
        "self.lamp_bar.control",
        "控制MOSS五路流水灯。action=start_flow 开启循环流水效果；stop_flow 停止并熄灭；"
        "get_status 查询运行状态和输出错误。灯效在后台运行。",
        PropertyList({Property("action", kPropertyTypeString)}),
        [&lights](const PropertyList& properties) -> ToolResult {
            const auto action = properties["action"].value<std::string>();
            if (action == "get_status") {
                return LightStatus(lights, false);
            }
            if (action != "start_flow" && action != "stop_flow") {
                return std::unexpected("action 必须是 start_flow、stop_flow 或 get_status");
            }
            auto result = lights.SetFlowing(action == "start_flow");
            if (!result) {
                return std::unexpected(result.error());
            }
            return std::string(action == "start_flow" ? "流水灯已开启" : "流水灯已停止并熄灭");
        });
    server.AddTool("self.lamp_eye.control",
                   "控制MOSS眼灯。action=turn_on 常亮；turn_off 关闭；start_breathing 开始呼吸；"
                   "pause_breathing 暂停并保持当前亮度；resume_breathing 恢复已暂停的呼吸；"
                   "stop_breathing 停止并熄灭；get_status 查询模式、PWM输出值和错误。"
                   "常亮、关闭、呼吸模式互斥，灯效在后台运行。",
                   PropertyList({Property("action", kPropertyTypeString)}),
                   [&lights](const PropertyList& properties) -> ToolResult {
                       const auto action = properties["action"].value<std::string>();
                       if (action == "get_status") {
                           return LightStatus(lights, true);
                       }
                       if (action == "resume_breathing") {
                           auto result = lights.ResumeBreathing();
                           if (!result) {
                               return std::unexpected(result.error());
                           }
                           return std::string("眼灯已恢复呼吸");
                       }
                       using Mode = MossLights::EyeMode;
                       Mode mode;
                       if (action == "turn_on") {
                           mode = Mode::On;
                       } else if (action == "turn_off" || action == "stop_breathing") {
                           mode = Mode::Off;
                       } else if (action == "start_breathing") {
                           mode = Mode::Breathing;
                       } else if (action == "pause_breathing") {
                           mode = Mode::Paused;
                       } else {
                           return std::unexpected("未知眼灯动作，请使用工具描述中的 action");
                       }
                       auto result = lights.SetEyeMode(mode);
                       if (!result) {
                           return std::unexpected(result.error());
                       }
                       return std::string("眼灯模式已切换为 ") + EyeModeName(mode);
                   });
}
