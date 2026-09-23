#pragma once

#include <BasicUsageEnvironment.hh>
#include <RTSPServer.hh>

#include "AudioFrame.hpp"
#include "VideoCodec.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class AnnexBFrameQueue;
class AudioAccessUnitQueue;
class RtspPublishSink;
class RtspAudioPublishSink;

// 一个端口上的多路 RTSP 发布服务。
// 调用方先 addStream() 注册所有 URL，再调用一次 start()；运行中不支持增删 URL。
// 所有 URL 共享同一个 live555 事件循环和认证配置。每路各自拥有视频 Annex-B queue、可选
// AAC audio queue，以及相互独立的 video/audio track 客户端计数。
class Live555RtspServer {
public:
    /*
     * 一个 URL 内可选的 AAC-LC audio track 配置。
     *
     * encoded 描述由 RtspAudioPublishSink 从 AudioPipeline 的实际采集格式自动取得；Server
     * 用它建立 SDP。所有 URL 共享同一个 AAC EncoderNode，仅复制 shared_ptr 到各 audio queue。
     */
    struct AacAudioTrackConfig {
        /* false 时该 URL 仅包含 video track。 */
        bool enabled = false;
        AudioEncodedStreamInfo encoded;
    };

    struct StreamConfig {
        /* URL path，例如 "main" 对应 rtsp://host:8554/main。 */
        std::string streamName;
        /* 本 URL 的视频编码格式。 */
        VideoCodec codec = VideoCodec::H264;
        /* 本 URL 是否额外挂载 AAC-LC audio track。 */
        AacAudioTrackConfig audio;
    };

    Live555RtspServer();
    ~Live555RtspServer();

    Live555RtspServer(const Live555RtspServer&) = delete;
    Live555RtspServer& operator=(const Live555RtspServer&) = delete;

    // 仅允许 start() 前调用。streamName 同时作为 RTSP URL path，例如 "main" -> /main。
    // Server 对两个 Sink 都只保留弱引用；IpcApp 仍负责持有其实际生命周期。
    // audio.enabled=true 时必须传入 audioPublishSink；同一个 Sink 可供 main/sub 共同使用。
    bool addStream(const StreamConfig& config,
                   const std::shared_ptr<RtspPublishSink>& publishSink,
                   const std::shared_ptr<RtspAudioPublishSink>& audioPublishSink = nullptr);

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

    /*
     * 向所有已启用且当前有 PLAY audio 客户端的 URL 分发一包 AAC-LC。
     *
     * 调用方交出的 packet 是 AudioPipeline 包池拥有的不可变 shared_ptr；本函数只复制
     * shared_ptr 到 /main、/sub 的独立 queue。没有 audio 客户端的 URL 直接忽略，不留历史音频。
     * packet 非 AAC 或没有任意 AAC track 时返回 false。
     */
    bool pushAacAccessUnit(EncodedAudioPacketPtr packet);

    bool isRunning() const;
    bool hasActiveClients(const std::string& streamName) const;
    std::string rtspURL(const std::string& streamName) const;
    std::string lastError() const;

private:
    // 每个 URL 的运行时状态。它不拥有 ServerMediaSession；session 由 m_server 接管。
    struct RtspPublishStream {
        RtspPublishStream(StreamConfig streamConfig,
                          std::weak_ptr<RtspPublishSink> streamPublishSink,
                          std::weak_ptr<RtspAudioPublishSink> streamAudioPublishSink);

        StreamConfig config;
        std::weak_ptr<RtspPublishSink> publishSink;
        std::weak_ptr<RtspAudioPublishSink> audioPublishSink;
        std::shared_ptr<AnnexBFrameQueue> frameQueue;
        std::shared_ptr<AudioAccessUnitQueue> audioQueue;
        mutable std::mutex clientStateMutex;
        /* video track 的 PLAY 客户端数量；仅它驱动视频 PublishSink。 */
        unsigned activeClientCount = 0;
        /* audio track 的 PLAY 客户端数量；仅它控制 audioQueue 是否接收新 AAC 包。 */
        unsigned activeAudioClientCount = 0;
        std::string url;
    };

    using PublishStreamPtr = std::shared_ptr<RtspPublishStream>;

    void rtspThreadMain();
    bool setupLive555();
    void cleanupLive555();
    void onClientPlaybackStateChanged(const PublishStreamPtr& stream, bool started);
    void onAudioClientPlaybackStateChanged(const PublishStreamPtr& stream, bool started);
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
    /*
     * 运行中的 URL 不允许增删。addStream() 时把带 AAC track 的 URL 预先收集到这里，
     * 让每个 AAC access unit 的分发不必临时构造 vector/申请堆内存。
     */
    std::vector<PublishStreamPtr> m_aacPublishStreams;
    std::string m_username;
    std::string m_password;
    std::string m_lastError;
    unsigned short m_rtspPort = 8554;
};
