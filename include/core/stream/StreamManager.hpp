#pragma once

#include "FrameHub.hpp"
#include "Stream.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// StreamManager 管理多路 Stream 的生命周期，以及解码后裸帧到 FrameHub 的发布。
// 每个 Stream 唯一对应一个 FrameHub；发布循环被 Stream 的 ready queue 唤醒后，
// 会排空所有 Stream 当前可取出的 FramePacket，再逐帧 publish。publish 运行在唯一线程，
// 因此 Sink::onFrame() 必须快速接管 FramePacket/lease 并返回，不能在其中做阻塞工作。
class StreamManager {
public:
    enum class StreamState {
        Created,
        Ready,
        Starting,
        Streaming,
        Stopping,
        Stopped,
        Deleting,
        Error,
    };

    struct StreamSlot {
        std::shared_ptr<Stream> stream;
        StreamState state = StreamState::Created;
        uint64_t runGeneration = 0;
        std::string lastError;
    };

    StreamManager();
    ~StreamManager();

    StreamManager(const StreamManager&) = delete;
    StreamManager& operator=(const StreamManager&) = delete;

    // StreamManager 内部单调分配 streamId。调用方先构造具体 Stream（如 RtspStream），
    // addStream 成功后返回 >= 0 的 streamId，失败返回 -1。
    int addStream(std::shared_ptr<Stream> stream);
    bool delStream(int streamId);

    bool addFrameSink(int streamId, std::shared_ptr<Sink> sink);

    // 返回 true 只表示控制请求已接受；底层网络连接/断开完成由 Stream 自己异步处理。
    bool startStream(int streamId);
    bool stopStream(int streamId);
    bool startAllStreams();
    void stopAllStreams();

    // 启停“解码完成帧 -> FrameHub”的统一发布循环。
    void startPublishing();
    void shutdownPublishing();

    // 查询单路运行状态。lastError 非空时写入该路的最近异步错误；返回 false 表示 streamId 不存在。
    bool getStreamState(int streamId, StreamState& state, std::string* lastError = nullptr) const;
    std::string lastError() const;

private:
    void runPublishLoop();
    bool publishReadyFrames();
    void notifyFrameReady();
    void notifyStreamRuntimeState(int streamId,
                                  uint64_t generation,
                                  Stream::RuntimeState state,
                                  const std::string& message);
    void applyPendingStreamRuntimeStates();
    int allocateStreamIdLocked();
    static uint64_t nextRunGeneration(uint64_t currentGeneration);
    void setError(const std::string& message);
    void clearError();

private:
    mutable std::mutex m_streamChangeMutex;
    int m_nextStreamId = 0;
    std::unordered_map<int, StreamSlot> m_streamMap;
    std::unordered_map<int, std::shared_ptr<FrameHub>> m_frameHubMap;

    std::mutex m_publishMutex;
    std::condition_variable m_publishCv;
    // 以下状态均只在 m_publishMutex 保护下访问。ready 事件用 bool 合并，避免多路流
    // 每帧都重复唤醒发布线程；发布期间到达的新帧会使下一轮 wait() 立即返回。
    bool m_hasPendingFrames = false;
    bool m_stopRequested = false;
    bool m_running = false;
    std::thread m_publishThread;

    struct PendingStreamRuntimeState {
        int streamId = -1;
        uint64_t generation = 0;
        Stream::RuntimeState state = Stream::RuntimeState::Error;
        std::string message;
    };
    std::deque<PendingStreamRuntimeState> m_pendingStreamRuntimeStates;

    mutable std::mutex m_errorMutex;
    std::string m_lastError;
};
