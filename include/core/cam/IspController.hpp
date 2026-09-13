#pragma once

// RV1126B ISP/AIQ 生命周期控制。
// 该类不暴露 Rockchip vendor 类型，避免上层代码依赖 AIQ SDK 细节。
class IspController {
public:
    IspController() = default;
    ~IspController();

    IspController(const IspController&) = delete;
    IspController& operator=(const IspController&) = delete;

    bool start();

    // 调用者必须先停止所有 V4L2 VPSS 节点；随后才允许停止 AIQ/3A。
    void stop();

private:
    void* m_context = nullptr;
    bool m_aiqStarted = false;
};
