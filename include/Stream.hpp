#pragma once

#include "DmaBufferPool.hpp"
#include "VideoFrame.hpp"
#include "MppTypes.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

class RgaEngine;
class StreamManager;

// Stream 表示一路网络/文件等非本地摄像头视频源。
// 派生类负责接收压缩码流；基类负责异步解码、稳定化 copy，以及把最新裸帧交给
// StreamManager。外部永远不会接触 live555/MPP 的临时内存。
class Stream {
public:
    // 这是 source 的运行状态，不等同于 StreamManager 的生命周期状态。派生类在底层
    // 协议真正进入播放或发生异步错误时上报，Manager 再据此更新自己的状态机。
    enum class RuntimeState {
        Connecting,
        Streaming,
        Stopped,
        Error,
    };

    using RuntimeStateCallback = std::function<void(uint64_t generation,
                                                    RuntimeState state,
                                                    const std::string& message)>;

    virtual ~Stream();

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // 启停底层 source。RtspStream 由 StreamManager 调用这两个接口。
    virtual bool start() = 0;
    virtual bool stop() = 0;

    // 非阻塞取走最新的稳定裸帧。返回 false 表示当前没有新帧。
    // packet 的 lease 会在最后一个使用者释放后，把 DMA buffer 归还给对应 pool。
    bool tryGetFrame(FramePacket& packet);

    int streamId() const;
    std::string lastError() const;

protected:
    // readyQueue 只保存尚未被 StreamManager 取走的稳定裸帧。RtspStream 默认
    // 传入 2，以吸收轻微调度抖动，同时保持显示追赶最新画面；录像不经过此队列。
    explicit Stream(size_t readyQueueCapacity);

    // 派生类从网络回调进入这里。data 只需要在本函数返回前有效；DecodeWorker 会复制。后续如果加录像应该从这里入手
    void onPacket(MppCodec codec, const uint8_t* data, size_t size, uint64_t timestampUs);

    bool startDecodeWorker();
    void stopDecodeWorker();
    void clearDecodePackets();

    void setError(const std::string& message);
    void clearError();
    void reportRuntimeState(RuntimeState state, const std::string& message = {});

private:
    friend class StreamManager;

    class DecodeWorker;

    struct OutputLayout {
        int width = 0;
        int height = 0;
        int stride = 0;
        int heightStride = 0;
        PixelFormat format = PixelFormat::Unknown;

        bool matches(const VideoFrame& frame) const;
    };

    // MPP 回调仅在 DecodeWorker 线程调用。它把 MPP 临时帧 RGA copy 到当前代 pool，
    // 并返回一个可跨线程持有的 FramePacket。
    bool makeStableFramePacket(const VideoFrame& decodedFrame,
                               RgaEngine& rga,
                               FramePacket& packet);
    bool ensureOutputPool(const VideoFrame& decodedFrame);
    bool discardPendingReadyFrame();
    void clearReadyFrames();
    void enqueueDecodedFrame(FramePacket packet);
    void recordDroppedFrames(size_t readyQueueFrames, size_t poolExhaustedFrames);
    void logDroppedFrameStatistics(bool force);
    void resetDroppedFrameStatistics();

    void setStreamId(int streamId);
    void setFrameReadyCallback(std::function<void()> callback);
    void setRuntimeStateCallback(RuntimeStateCallback callback);
    void setRunGeneration(uint64_t generation);

private:
    std::unique_ptr<DecodeWorker> m_decodeWorker;

    const size_t m_readyQueueCapacity;

    // 分辨率/layout 改变时创建新 pool 并替换本指针；旧 FramePacket 的 lease 持有旧 pool，
    // 所以不会在 Sink 仍使用旧 dma-buf 时提前释放。
    std::shared_ptr<DmaBufferPool> m_activePool;
    OutputLayout m_outputLayout;

    mutable std::mutex m_readyMutex;
    std::deque<FramePacket> m_readyQueue;
    std::function<void()> m_frameReadyCallback;

    mutable std::mutex m_runtimeStateMutex;
    RuntimeStateCallback m_runtimeStateCallback;
    uint64_t m_runGeneration = 0;

    // 统计的是稳定裸帧阶段的丢弃，不包含 RTSP/UDP 网络层和压缩 NALU 队列的丢包。
    // 日志仅在发生丢帧时最多每秒输出一次，避免反压时刷屏。
    mutable std::mutex m_dropStatisticsMutex;
    uint64_t m_readyQueueDroppedFrames = 0;
    uint64_t m_poolExhaustedDroppedFrames = 0;
    std::chrono::steady_clock::time_point m_lastDropStatisticsLog {};

    int m_streamId = -1;
    uint64_t m_outputSequence = 0;

    mutable std::mutex m_errorMutex;
    std::string m_lastError;
};
