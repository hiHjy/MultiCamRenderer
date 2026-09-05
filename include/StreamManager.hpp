#pragma once

#include "FrameHub.hpp"
#include "Stream.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// StreamManager 管理多路 Stream 的生命周期，以及解码后裸帧到 FrameHub 的发布。
// 每个 Stream 唯一对应一个 FrameHub；发布循环被 Stream 的 ready queue 唤醒后，
// 会排空所有 Stream 当前可取出的 FramePacket，再逐帧 publish。
class StreamManager {
public:
    enum class StreamState {
        Created,
        Ready,
        Streaming,
        Stopped,
        Deleting,
        Error,
    };

    struct StreamSlot {
        std::shared_ptr<Stream> stream;
        StreamState state = StreamState::Created;
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

    std::string lastError() const;

private:
    void runPublishLoop();
    bool publishReadyFrames();
    void notifyFrameReady();
    int allocateStreamIdLocked();
    void setError(const std::string& message);
    void clearError();

private:
    std::mutex m_streamChangeMutex;
    int m_nextStreamId = 0;
    std::unordered_map<int, StreamSlot> m_streamMap;
    std::unordered_map<int, std::shared_ptr<FrameHub>> m_frameHubMap;

    std::mutex m_publishMutex;
    std::condition_variable m_publishCv;
    std::atomic_bool m_hasPendingFrames {false};
    std::atomic_bool m_stopRequested {false};
    std::atomic_bool m_running {false};
    std::thread m_publishThread;

    mutable std::mutex m_errorMutex;
    std::string m_lastError;
};
