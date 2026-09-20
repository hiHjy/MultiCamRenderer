#include "LocalAgent.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>

namespace {

std::string compactJson(const std::string& text)
{
    std::string compact;
    compact.reserve(text.size());
    for (unsigned char ch : text) {
        if (std::isspace(ch) == 0)
            compact += static_cast<char>(ch);
    }
    return compact;
}

bool parseNonNegativeInt(const std::string& text, int& value)
{
    if (text.empty())
        return false;

    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc {} && result.ptr == end && value >= 0;
}

bool parseCameraIdCall(const std::string& json, const std::string& name, int& cameraId)
{
    const std::string prefix =
        "{\"type\":\"tool_call\",\"name\":\"" + name + "\",\"arguments\":{\"camera_id\":";
    constexpr const char suffix[] = "}}";
    if (json.rfind(prefix, 0) != 0 || json.size() <= prefix.size() + 2 ||
        json.compare(json.size() - 2, 2, suffix) != 0) {
        return false;
    }

    const std::string value = json.substr(prefix.size(), json.size() - prefix.size() - 2);
    if (!parseNonNegativeInt(value, cameraId))
        return false;
    return true;
}

} // namespace

LocalAgent::LocalAgent(AgentModel& model, AgentToolExecutor& tools)
    : m_model(model)
    , m_tools(tools)
{
}

bool LocalAgent::handleRequest(const std::string& userRequest, std::string& reply)
{
    ToolCall call;
    if (!requestToolCall(userRequest, call))
        return false;

    std::string resultJson;
    if (!executeToolCall(call, resultJson))
        return false;

    const std::string finalPrompt =
        "你是本地 IPC Agent。用户请求：" + userRequest +
        "\n工具已由 C++ 执行，真实返回结果：" + resultJson +
        "\n请只用一句简洁中文告诉用户真实结果；不要输出 JSON、Markdown、工具名或推测。";
    if (!m_model.generate(finalPrompt, reply)) {
        m_lastError = "生成最终回答失败: " + m_model.lastError();
        return false;
    }

    m_lastError.clear();
    return true;
}

const std::string& LocalAgent::lastError() const
{
    return m_lastError;
}

bool LocalAgent::requestToolCall(const std::string& userRequest, ToolCall& call)
{
    const std::string basePrompt =
        "你是本地 IPC Agent 的工具选择器。可调用的工具只有：\n"
        "1. get_camera_status(camera_id: integer)：查询指定摄像头状态。\n"
        "2. set_night_mode(camera_id: integer, enabled: boolean)：开关指定摄像头夜视。\n"
        "3. get_system_info(category: string)：查询系统信息；内存类别必须为 memory。\n"
        "只能输出一行 JSON，不能输出 Markdown、解释或其他文字。JSON 必须完全符合："
        "{\"type\":\"tool_call\",\"name\":\"工具名\",\"arguments\":{...}}。\n"
        "用户请求：" + userRequest;

    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::string prompt = attempt == 0 ? basePrompt :
            "上一条工具调用未通过参数校验。不要臆造参数；现在严格按协议重新输出。\n" + basePrompt;
        std::string modelReply;
        if (!m_model.generate(prompt, modelReply)) {
            m_lastError = "请求工具调用失败: " + m_model.lastError();
            return false;
        }
        if (parseToolCall(modelReply, call))
            return true;
    }

    m_lastError = "模型连续两次没有生成允许的工具调用";
    return false;
}

bool LocalAgent::parseToolCall(const std::string& modelReply, ToolCall& call)
{
    // 故意只接受固定字段顺序和白名单参数；模型多写一个字段也会在这里被拒绝。
    const std::string json = compactJson(modelReply);
    int cameraId = -1;
    if (parseCameraIdCall(json, "get_camera_status", cameraId)) {
        call.tool = Tool::CameraStatus;
        call.cameraId = cameraId;
        return true;
    }

    const std::string nightPrefix =
        "{\"type\":\"tool_call\",\"name\":\"set_night_mode\",\"arguments\":{\"camera_id\":";
    constexpr const char enabledMarker[] = ",\"enabled\":";
    constexpr const char nightSuffix[] = "}}";
    if (json.rfind(nightPrefix, 0) == 0 && json.size() > nightPrefix.size() + 2 &&
        json.compare(json.size() - 2, 2, nightSuffix) == 0) {
        const size_t marker = json.find(enabledMarker, nightPrefix.size());
        if (marker != std::string::npos) {
            int cameraId = -1;
            const std::string idText = json.substr(nightPrefix.size(), marker - nightPrefix.size());
            const std::string enabledText = json.substr(marker + sizeof(enabledMarker) - 1,
                                                        json.size() - marker - (sizeof(enabledMarker) - 1) - 2);
            if (parseNonNegativeInt(idText, cameraId) &&
                (enabledText == "true" || enabledText == "false")) {
                call.tool = Tool::SetNightMode;
                call.cameraId = cameraId;
                call.enabled = enabledText == "true";
                return true;
            }
        }
    }

    if (json == "{\"type\":\"tool_call\",\"name\":\"get_system_info\","
                "\"arguments\":{\"category\":\"memory\"}}") {
        call.tool = Tool::MemoryInfo;
        return true;
    }
    return false;
}

bool LocalAgent::executeToolCall(const ToolCall& call, std::string& resultJson)
{
    bool ok = false;
    switch (call.tool) {
    case Tool::CameraStatus:
        ok = m_tools.getCameraStatus(call.cameraId, resultJson);
        break;
    case Tool::SetNightMode:
        ok = m_tools.setNightMode(call.cameraId, call.enabled, resultJson);
        break;
    case Tool::MemoryInfo:
        ok = m_tools.getMemoryInfo(resultJson);
        break;
    }

    if (!ok)
        m_lastError = "工具执行失败: " + m_tools.lastError();
    return ok;
}
