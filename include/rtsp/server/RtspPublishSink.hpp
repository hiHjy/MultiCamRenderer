#pragma once

#include "MppEncoder.hpp"
#include "OsdRenderer.hpp"
#include "Sink.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class Live555RtspServer;

// Server 已在 live555 线程完成客户端数更新后，交给 PublishSink 的边界事件。
// Sink 只置 worker 控制标志，绝不在 live555 线程直接调用 MPP。
enum class RtspClientPlaybackEvent {
    FirstClientStarted,      // 已 PLAY 客户端数：0 -> 1
    AdditionalClientStarted, // 已 PLAY 客户端数：N -> N + 1，N >= 1
    LastClientStopped,       // 已 PLAY 客户端数：1 -> 0
};

// 一路裸帧到 RTSP 的发布消费者。
// onFrame() 只保存最新待编码帧；MPP 编码及 live555 入队均在专用 worker 中完成。
// 普通模式直接借用 CamManager 的 DMA-BUF 到 sendFrame() 返回，期间 FrameLease 会保护
// V4L2 buffer 不被提前 QBUF。启用 OSD 时，worker 会先 RGA copy 到自己的单块 NV12
// DMA-BUF，再叠字/画框并送 MPP；由于 sendFrame() 同步完成，这一块 buffer 可安全复用。
class RtspPublishSink final : public Sink {
public:
    struct Config {
        std::string streamName;
        MppEncoderConfig encoderConfig;

        // 启用后，worker 在 MPP 编码前把 VPSS 的 NV12 复制到私有输出 DMA-BUF，
        // 再叠加文字/检测框。默认关闭，保持其他 PublishSink 使用方原有的零额外 copy 行为。
        bool enableOsd = false;
        OsdRendererConfig osdConfig;
    };

    RtspPublishSink(Live555RtspServer& server, Config config);
    ~RtspPublishSink() override;

    RtspPublishSink(const RtspPublishSink&) = delete;
    RtspPublishSink& operator=(const RtspPublishSink&) = delete;

    // 可由 live555 的客户端活动回调调用；函数只更新 worker 状态，不阻塞 RTSP 事件线程。
    // active=true 时 worker 开始接收裸帧，并以第一张真实图像初始化编码器；
    // 第一张接收的图像从新的编码序列开始。
    // active=false 时立即丢弃待编码帧并 deinit 编码器。
    void setActive(bool active);

    // 请求编码 worker 在下一次 sendFrame() 前强制 IDR。可由 live555 事件线程调用；
    // 多次请求合并为一次，绝不在调用线程直接操作 MPP。
    void requestKeyFrame();

    // 首个客户端启动编码，后来客户端请求下一帧 IDR，最后一个客户端离开时停止编码。
    // 应用层无需重复配置这些固定推流策略。
    void onClientPlaybackEvent(RtspClientPlaybackEvent event);

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
