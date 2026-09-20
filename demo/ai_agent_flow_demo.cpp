#include <iostream>
#include <string>

#include "LocalAgent.hpp"

namespace {

class ScriptedModel final : public AgentModel {
public:
    bool generate(const std::string& prompt, std::string& response) override
    {
        if (prompt.find("工具选择器") != std::string::npos) {
            const size_t requestPos = prompt.rfind("用户请求：");
            const std::string request = requestPos == std::string::npos ? prompt :
                prompt.substr(requestPos + std::string("用户请求：").size());
            if (request.find("夜视") != std::string::npos) {
                response = "{\"type\":\"tool_call\",\"name\":\"set_night_mode\","
                           "\"arguments\":{\"camera_id\":0,\"enabled\":true}}";
            } else if (prompt.find("内存") != std::string::npos) {
                response = "{\"type\":\"tool_call\",\"name\":\"get_system_info\","
                           "\"arguments\":{\"category\":\"memory\"}}";
            } else {
                response = "{\"type\":\"tool_call\",\"name\":\"get_camera_status\","
                           "\"arguments\":{\"camera_id\":0}}";
            }
            return true;
        }

        const size_t requestPos = prompt.find("用户请求：");
        const std::string request = requestPos == std::string::npos ? prompt :
            prompt.substr(requestPos + std::string("用户请求：").size());
        if (request.find("推流") != std::string::npos) {
            response = "摄像头 0 正在推流，帧率 25。";
        } else if (request.find("夜视") != std::string::npos) {
            response = "摄像头 0 的夜视已开启。";
        } else {
            response = "当前可用内存 700MB，交换空间 0MB。";
        }
        return true;
    }

    const std::string& lastError() const override { return m_lastError; }

private:
    std::string m_lastError;
};

class MockTools final : public AgentToolExecutor {
public:
    bool getCameraStatus(int cameraId, std::string& resultJson) override
    {
        resultJson = "{\"camera_id\":" + std::to_string(cameraId) +
                     ",\"streaming\":true,\"fps\":25}";
        return true;
    }

    bool setNightMode(int cameraId, bool enabled, std::string& resultJson) override
    {
        resultJson = "{\"camera_id\":" + std::to_string(cameraId) +
                     ",\"night_mode\":" + (enabled ? "true" : "false") +
                     ",\"applied\":true}";
        return true;
    }

    bool getMemoryInfo(std::string& resultJson) override
    {
        resultJson = "{\"memory_available_mb\":700,\"swap_available_mb\":0}";
        return true;
    }

    const std::string& lastError() const override { return m_lastError; }

private:
    std::string m_lastError;
};

void run(LocalAgent& agent, const std::string& request)
{
    std::string reply;
    std::cout << "user: " << request << std::endl;
    if (agent.handleRequest(request, reply))
        std::cout << "agent: " << reply << "\n" << std::endl;
    else
        std::cerr << "agent failed: " << agent.lastError() << "\n" << std::endl;
}

} // namespace

int main()
{
    ScriptedModel model;
    MockTools tools;
    LocalAgent agent(model, tools);

    run(agent, "请查询摄像头 0 是否正在推流。");
    run(agent, "请打开摄像头 0 的夜视模式。");
    run(agent, "请查询设备当前内存余量。");
    return 0;
}
