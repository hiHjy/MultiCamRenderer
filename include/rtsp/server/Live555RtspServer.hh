#pragma once

#include <BasicUsageEnvironment.hh>
#include <RTSPServer.hh>

#include "VideoCodec.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

class AnnexBFrameQueue;

// 一个端口上的多路 RTSP 发布服务。
// 调用方先 addStream() 注册所有 URL，再调用一次 start()；运行中不支持增删 URL。
// 所有 URL 共享同一个 live555 事件循环和认证配置，每路各自拥有 Annex-B 队列与客户端计数。
class Live555RtspServer {
public:
    struct StreamConfig {
        std::string streamName;
        VideoCodec codec = VideoCodec::H264;
        // 仅在已 PLAY 客户端数发生 0->1 或 1->0 变化时调用。
        // 回调运行在 live555 事件线程；应只投递轻量控制命令，不能阻塞。
        std::function<void(bool active)> onClientActiveChanged;
        // 仅在已有客户端播放时，又有一个新客户端完成 PLAY 时调用。
        // 典型用途是请求编码器下一帧 IDR，让新客户端立即获得可解码边界。
        // 回调运行在 live555 事件线程；不得直接操作 MPP。
        std::function<void()> onAdditionalClientStarted;
    };

    Live555RtspServer();
    ~Live555RtspServer();

    Live555RtspServer(const Live555RtspServer&) = delete;
    Live555RtspServer& operator=(const Live555RtspServer&) = delete;

    // 仅允许 start() 前调用。streamName 同时作为 RTSP URL path，例如 "main" -> /main。
    bool addStream(const StreamConfig& config);

    // 用户名和密码按端口生效，所有已注册 URL 共用。
    bool start(unsigned short rtspPort, const std::string& username, const std::string& password);
    void stop();

    // data 可以是一个 NALU 或包含多个 NALU 的 Annex-B access unit；函数会立即复制数据。
    // 该路没有已 PLAY 客户端时直接丢弃，避免向新客户端发送历史码流。
    // streamName 不存在时返回 false，其他情况返回 true（即使本次因无客户端被丢弃）。
    bool pushAnnexBFrame(const std::string& streamName,
                         const uint8_t* data,
                         size_t size,
                         uint64_t timestampUs);

    bool isRunning() const;
    bool hasActiveClients(const std::string& streamName) const;
    std::string rtspURL(const std::string& streamName) const;
    std::string lastError() const;

private:
    // 每个 URL 的运行时状态。它不拥有 ServerMediaSession；session 由 m_server 接管。
    struct RtspPublishStream {
        explicit RtspPublishStream(StreamConfig streamConfig);

        StreamConfig config;
        std::shared_ptr<AnnexBFrameQueue> frameQueue;
        mutable std::mutex clientStateMutex;
        unsigned activeClientCount = 0;
        std::string url;
    };

    using PublishStreamPtr = std::shared_ptr<RtspPublishStream>;

    void rtspThreadMain();
    bool setupLive555();
    void cleanupLive555();
    void onClientPlaybackStateChanged(const PublishStreamPtr& stream, bool started);
    PublishStreamPtr findStream(const std::string& streamName) const;
    void setErrorLocked(const std::string& message);
    static void stopEventCallback(void* clientData);

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_startCv;
    std::thread m_rtspThread;

    std::atomic_bool m_running {false};
    bool m_startFinished = false;
    bool m_startOk = false;

#if LIVEMEDIA_LIBRARY_VERSION_INT >= 1700000000
    EventLoopWatchVariable m_eventLoopWatchVariable {0};
#else
    // 2021.05.03 的 doEventLoop() 使用 char volatile* 退出标志。
    char volatile m_eventLoopWatchVariable = 0;
#endif
    TaskScheduler* m_scheduler = nullptr;
    UsageEnvironment* m_env = nullptr;
    UserAuthenticationDatabase* m_authDb = nullptr;
    RTSPServer* m_server = nullptr;
    EventTriggerId m_stopTrigger = 0;

    std::unordered_map<std::string, PublishStreamPtr> m_streamMap;
    std::string m_username;
    std::string m_password;
    std::string m_lastError;
    unsigned short m_rtspPort = 8554;
};
