#pragma once

#include "VideoFrame.hpp"
#include "hw/MppTypes.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

class StreamManager;

// Stream 表示一路网络/文件等非本地摄像头的视频源。
// 派生类负责接收并组出完整的压缩 access unit；基类后续负责把解码后的裸帧
// 按顺序放入 ready queue，供 StreamManager 统一发布给对应 FrameHub。
class Stream {
public:
    virtual ~Stream();

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // 启停底层 source。RtspStream 的实现将负责 live555 session 等资源。
    virtual bool start() = 0;
    virtual bool stop() = 0;

    // 非阻塞地取出一帧已经解码完成的裸帧。
    // packet 必须整体传递，不能只取 VideoFrame；lease 负责保护输出 DMA buffer
    // 在 FrameHub/Sink 使用期间不会被提前归还给 Stream 的输出 pool。
    bool tryGetFrame(FramePacket& packet);

    int streamId() const;
    std::string lastError() const;

protected:
    explicit Stream(MppCodec codec);

    // 解码 worker 完成稳定化 copy 后调用。packet 按 FIFO 进入 ready queue，
    // 然后通知 StreamManager 的发布循环。
    bool enqueueDecodedFrame(FramePacket packet);
    void setError(const std::string& message);
    void clearError();

private:
    friend class StreamManager;

    // streamId 仅由 StreamManager 分配，避免外部复用 id 时把旧帧发布到新 hub。
    void bindStreamId(int streamId);
    void setFrameReadyCallback(std::function<void()> callback);

    // 每路 Stream 独占一个 DecodeWorker；MPP context 只由这条 worker 线程访问。
    class DecodeWorker;
    std::unique_ptr<DecodeWorker> m_decodeWorker;

    MppCodec m_codec = MppCodec::H264;
    int m_streamId = -1;

    mutable std::mutex m_readyMutex;
    std::queue<FramePacket> m_readyQueue;
    std::function<void()> m_frameReadyCallback;

    mutable std::mutex m_errorMutex;
    std::string m_lastError;
};
