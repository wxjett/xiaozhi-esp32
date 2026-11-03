#include "moss_motor.h"

#include "config.h"
#include "mcp_server.h"

namespace {
std::optional<MossMotor::Axis> ParseAxis(const std::string& name) {
    if (name == "pitch") {
        return MossMotor::Axis::Pitch;
    }
    if (name == "yaw") {
        return MossMotor::Axis::Yaw;
    }
    return std::nullopt;
}
}  // namespace

void RegisterMossMotorTools(MossMotor& motor) {
    auto& server = McpServer::GetInstance();
    server.AddTool(
        "self.motor.control",
        "MOSS电机相对转动。motor=pitch: 正角度向下、负角度向上；motor=yaw: 正角度向右、负角度向左。"
        "同一轴的命令依次执行，两轴可同时运动。返回仅表示命令已接受，不表示已转完。"
        "这是开环步进控制，没有归零、位置反馈或机械限位；单次角度范围不是机械安全行程。",
        PropertyList({Property("motor", kPropertyTypeString),
                      Property("angle", kPropertyTypeInteger, -MOTOR_MAX_RELATIVE_ANGLE_DEG,
                               MOTOR_MAX_RELATIVE_ANGLE_DEG)}),
        [&motor](const PropertyList& properties) -> ToolResult {
            const auto axis = ParseAxis(properties["motor"].value<std::string>());
            if (!axis) {
                return std::unexpected("motor 必须是 pitch 或 yaw");
            }
            const int angle = properties["angle"].value<int>();
            auto result = motor.MoveRelative(*axis, angle);
            if (!result) {
                return std::unexpected(result.error());
            }
            return angle == 0 ? std::string("角度为0，无需运动")
                              : std::string("运动命令已接受，等待或正在执行");
        });
    server.AddTool("self.motor.stop",
                   "停止MOSS指定电机轴，清空该轴待执行命令并关闭励磁。motor只能为pitch或yaw。",
                   PropertyList({Property("motor", kPropertyTypeString)}),
                   [&motor](const PropertyList& properties) -> ToolResult {
                       const auto axis = ParseAxis(properties["motor"].value<std::string>());
                       if (!axis) {
                           return std::unexpected("motor 必须是 pitch 或 yaw");
                       }
                       auto result = motor.Stop(*axis);
                       if (!result) {
                           return std::unexpected(result.error());
                       }
                       return std::string("该轴已停止，待执行命令已清空");
                   });
    server.AddTool(
        "self.motor.get_status",
        "查询MOSS两轴的运动状态、排队数量、剩余半步数和GPIO错误。executed_half_"
        "steps仅为输出相序计数，"
        "不是实际位置或绝对角度。",
        PropertyList(), [&motor](const PropertyList&) -> ToolResult {
            const auto status = motor.GetStatus();
            CJsonUniquePtr root(cJSON_CreateObject());
            if (!root) {
                return std::unexpected("无法创建电机状态响应");
            }
            cJSON_AddBoolToObject(root.get(), "ready", status.ready);
            const char* names[] = {"pitch", "yaw"};
            for (size_t i = 0; i < status.axes.size(); ++i) {
                const auto& axis = status.axes[i];
                auto node = cJSON_AddObjectToObject(root.get(), names[i]);
                if (!node) {
                    return std::unexpected("无法创建轴状态响应");
                }
                cJSON_AddBoolToObject(node, "moving", axis.moving);
                cJSON_AddBoolToObject(node, "energized", axis.energized);
                cJSON_AddNumberToObject(node, "queued_commands", axis.queued_commands);
                cJSON_AddNumberToObject(node, "remaining_half_steps", axis.remaining_half_steps);
                cJSON_AddNumberToObject(node, "executed_half_steps", axis.executed_half_steps);
                cJSON_AddStringToObject(node, "last_error", esp_err_to_name(axis.last_error));
            }
            return root.release();
        });
}
