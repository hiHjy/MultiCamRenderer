#pragma once

#include <string>

// 语言模型的最小抽象。它不知道摄像头、工具或权限，只负责把提示词变成文本回复。
class AgentModel {
public:
    virtual ~AgentModel() = default;

    // 同步完成一次推理。成功时将模型完整回复写入 response。
    virtual bool generate(const std::string& prompt, std::string& response) = 0;

    // 返回最近一次模型调用失败的原因。
    virtual const std::string& lastError() const = 0;
};
