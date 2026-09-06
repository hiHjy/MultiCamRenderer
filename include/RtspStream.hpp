#pragma once

#include "Stream.hpp"

#include <memory>
#include <string>

class Live555RtspClient;

// 一路 RTSP 视频源。codec 不从外部传入：live555 从 SDP 选中 H264/H265 后，
// 回调把实际 codec 交给 Stream 的 DecodeWorker 初始化对应 MPP decoder。
class RtspStream final : public Stream {
public:
    // readyQueueCapacity 是解码完成、等待 StreamManager 取走的裸帧数量。
    // 默认 2，用于吸收轻微调度抖动；实时显示仍优先保留最新帧。
    explicit RtspStream(std::string url, size_t readyQueueCapacity = 2);
    ~RtspStream() override;

    bool start() override;
    bool stop() override;

private:
    std::string m_url;
    std::unique_ptr<Live555RtspClient> m_client;
    bool m_started = false;
};
