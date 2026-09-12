#pragma once

#include "MppEncoder.hpp"
#include "Sink.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class Live555RtspServer;

// 一路裸帧到 RTSP 的发布消费者。
// onFrame() 只保存最新待编码帧；MPP 编码及 live555 入队均在专用 worker 中完成。
// 普通模式直接借用 CamManager 的 DMA-BUF 到 sendFrame() 返回，期间 FrameLease 会保护
// V4L2 buffer 不被提前 QBUF。AI 模式以后可在本类中改为私有 DMA pool，不影响上游接口。
class RtspPublishSink final : public Sink {
public:
    struct Config {
        std::string streamName;
        MppEncoderConfig encoderConfig;
    };

    RtspPublishSink(Live555RtspServer& server, Config config);
    ~RtspPublishSink() override;

    RtspPublishSink(const RtspPublishSink&) = delete;
    RtspPublishSink& operator=(const RtspPublishSink&) = delete;

    // 可由 live555 的客户端活动回调调用；函数只更新 worker 状态，不阻塞 RTSP 事件线程。
    // active=true 时 worker 初始化编码器，第一张接收的图像从新的编码序列开始；
    // active=false 时立即丢弃待编码帧并 deinit 编码器。
    void setActive(bool active);

    // 请求编码 worker 在下一次 sendFrame() 前强制 IDR。可由 live555 事件线程调用；
    // 多次请求合并为一次，绝不在调用线程直接操作 MPP。
    void requestKeyFrame();

    void onFrame(FramePacket packet) override;

    bool isActive() const;
    uint64_t droppedFrameCount() const;
    std::string lastError() const;

private:
    void workerMain();
    void setErrorLocked(const std::string& message);

private:
    Live555RtspServer& m_server;
    const Config m_config;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_workerThread;
    std::optional<FramePacket> m_pendingFrame;
    bool m_activeRequested = false;
    bool m_acceptingFrames = false;
    bool m_keyFrameRequested = false;
    bool m_stopping = false;
    uint64_t m_droppedFrames = 0;
    std::string m_lastError;
};
