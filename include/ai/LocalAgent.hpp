#pragma once

#include <string>

#include "AgentModel.hpp"
#include "AgentToolExecutor.hpp"

// 本地设备 Agent 的编排层：模型只能选择白名单工具，真实动作始终由 AgentToolExecutor 完成。
class LocalAgent {
public:
    LocalAgent(AgentModel& model, AgentToolExecutor& tools);

    // 处理一句用户请求。成功时 reply 是根据真实工具结果生成的自然语言回答。
    bool handleRequest(const std::string& userRequest, std::string& reply);

    const std::string& lastError() const;

private:
    enum class Tool {
        CameraStatus,
        SetNightMode,
        MemoryInfo,
    };

    struct ToolCall {
        Tool tool = Tool::CameraStatus;
        int cameraId = -1;
        bool enabled = false;
    };

    bool requestToolCall(const std::string& userRequest, ToolCall& call);
    bool parseToolCall(const std::string& modelReply, ToolCall& call);
    bool executeToolCall(const ToolCall& call, std::string& resultJson);

    AgentModel& m_model;
    AgentToolExecutor& m_tools;
    std::string m_lastError;
};
