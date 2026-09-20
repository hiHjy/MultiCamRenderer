#pragma once

#include <string>

// Agent 不直接操作硬件。所有有副作用的动作都必须由此接口的具体实现执行。
class AgentToolExecutor {
public:
    virtual ~AgentToolExecutor() = default;

    // 查询 cameraId 的状态；resultJson 必须由真实系统状态构造，不能相信模型生成的状态。
    virtual bool getCameraStatus(int cameraId, std::string& resultJson) = 0;

    // 设置 cameraId 的夜视模式；调用方负责检查 cameraId 和权限。
    virtual bool setNightMode(int cameraId, bool enabled, std::string& resultJson) = 0;

    // 查询设备内存信息；resultJson 由 /proc 或系统运行时状态构造。
    virtual bool getMemoryInfo(std::string& resultJson) = 0;

    virtual const std::string& lastError() const = 0;
};
