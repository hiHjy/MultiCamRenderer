#include "Live555RtspServer.hh"

#include "AacAudioSubsession.hh"
#include "AnnexBFrameQueue.hh"
#include "AudioAccessUnitQueue.hh"
#include "AudioAac.h"
#include "Log.hpp"
#include "RtspAudioPublishSink.hpp"
#include "RtspPublishSink.hpp"
#include "VideoSubsession.hh"

#include <utility>
#include <vector>

namespace {

// UDP 客户端直接消失时，只能等 RTCP RR 超时回收 session。官方默认 65 秒，
// 对实时发布队列过长；10 秒后回收即可停止无客户端时的无效编码入队。
constexpr unsigned kRtspClientReclamationSeconds = 10;

bool isValidStreamName(const std::string& streamName)
{
    // streamName 是 URL path 的一个段，禁止空白和路径分隔符，避免注册出歧义 URL。
    if (streamName.empty())
        return false;

    for (const unsigned char ch : streamName) {
        if (ch == '/' || ch == '\\' || ch <= ' ')
            return false;
    }
    return true;
}

const char* codecName(VideoCodec codec)
{
    return codec == VideoCodec::H264 ? "H264" : "H265";
}

bool isValidAacAudioConfig(const Live555RtspServer::AacAudioTrackConfig& audio)
{
    return !audio.enabled
        || (audio.encoded.codec == AUDIO_CODEC_AAC && audio.encoded.sourceFormat.sampleRate != 0
            && audio.encoded.sourceFormat.channels >= 1 && audio.encoded.sourceFormat.channels <= 2
            && audio.encoded.sourceFormat.sampleFormat == AUDIO_SAMPLE_FORMAT_S16_LE
            && audio.encoded.bitrate != 0 && audio.encoded.frameSamples == AUDIO_AAC_LC_FRAME_SAMPLES);
}

} // namespace

Live555RtspServer::RtspPublishStream::RtspPublishStream(
    StreamConfig streamConfig,
    std::weak_ptr<RtspPublishSink> streamPublishSink,
    std::weak_ptr<RtspAudioPublishSink> streamAudioPublishSink)
    : config(std::move(streamConfig)),
      publishSink(std::move(streamPublishSink)),
      audioPublishSink(std::move(streamAudioPublishSink)),
      frameQueue(std::make_shared<AnnexBFrameQueue>())
{
    if (config.audio.enabled) {
        audioQueue = std::make_shared<AudioAccessUnitQueue>();
    }
}

Live555RtspServer::Live555RtspServer() = default;

Live555RtspServer::~Live555RtspServer()
{
    stop();
}

bool Live555RtspServer::addStream(const StreamConfig& config,
                                  const std::shared_ptr<RtspPublishSink>& publishSink,
                                  const std::shared_ptr<RtspAudioPublishSink>& audioPublishSink)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running.load()) {
        setErrorLocked("RTSP Server 运行中，不能动态添加 stream");
        return false;
    }
    if (!isValidStreamName(config.streamName)) {
        setErrorLocked("无效的 RTSP streamName: " + config.streamName);
        return false;
    }
    if (m_streamMap.find(config.streamName) != m_streamMap.end()) {
        setErrorLocked("重复的 RTSP streamName: " + config.streamName);
        return false;
    }
    if (publishSink == nullptr) {
        setErrorLocked("RTSP stream 缺少 RtspPublishSink: " + config.streamName);
        return false;
    }
    if (!isValidAacAudioConfig(config.audio)) {
        setErrorLocked("RTSP stream AAC 配置无效: " + config.streamName);
        return false;
    }
    if (config.audio.enabled && (audioPublishSink == nullptr || !audioPublishSink->isReady())) {
        setErrorLocked("RTSP stream 缺少可用 RtspAudioPublishSink: " + config.streamName);
        return false;
    }

    const PublishStreamPtr stream = std::make_shared<RtspPublishStream>(config, publishSink, audioPublishSink);
    m_streamMap.emplace(config.streamName, stream);
    if (config.audio.enabled) {
        /* addStream() 只能在 start() 前调用；此表在 RTSP 运行期间保持不变。 */
        m_aacPublishStreams.push_back(stream);
    }
    return true;
}

bool Live555RtspServer::start(unsigned short rtspPort,
                              const std::string& username,
                              const std::string& password)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_running.load())
        return m_startOk;
    if (m_streamMap.empty()) {
        setErrorLocked("RTSP Server 未注册任何 stream");
        return false;
    }
    // 与 OnvifServer 的认证配置语义保持一致：账号和密码要么一起提供，要么一起关闭。
    // 否则 ONVIF 会拒绝启动、RTSP 却建立不完整认证库，两个入口的行为会分叉。
    if (username.empty() != password.empty()) {
        setErrorLocked("RTSP 认证必须同时提供用户名和密码，或两者均留空关闭认证");
        return false;
    }

    m_rtspPort = rtspPort == 0 ? 8554 : rtspPort;
    m_username = username;
    m_password = password;
    m_lastError.clear();
#if LIVEMEDIA_LIBRARY_VERSION_INT >= 1700000000
    m_eventLoopWatchVariable.store(0);
#else
    m_eventLoopWatchVariable = 0;
#endif
    m_startFinished = false;
    m_startOk = false;
    for (const auto& entry : m_streamMap) {
        const PublishStreamPtr& stream = entry.second;
        std::lock_guard<std::mutex> clientLock(stream->clientStateMutex);
        stream->activeClientCount = 0;
        stream->activeAudioClientCount = 0;
        stream->url.clear();
        stream->frameQueue->clear();
        if (stream->audioQueue != nullptr) {
            stream->audioQueue->clear();
        }
    }

    m_running.store(true);
    m_rtspThread = std::thread(&Live555RtspServer::rtspThreadMain, this);

    // 端口绑定、认证库和全部 ServerMediaSession 都在 live555 线程创建；等待它们完成，
    // 让 start() 的返回值真实表达每一路 URL 是否都已可用。
    m_startCv.wait(lock, [this] { return m_startFinished; });
    const bool started = m_startOk;
    lock.unlock();

    if (!started && m_rtspThread.joinable())
        m_rtspThread.join();
    return started;
}

void Live555RtspServer::stop()
{
    TaskScheduler* scheduler = nullptr;
    EventTriggerId stopTrigger = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running.load() && !m_rtspThread.joinable())
            return;

#if LIVEMEDIA_LIBRARY_VERSION_INT >= 1700000000
        m_eventLoopWatchVariable.store(1);
#else
        m_eventLoopWatchVariable = 1;
#endif
        scheduler = m_scheduler;
        stopTrigger = m_stopTrigger;
    }

    if (scheduler != nullptr && stopTrigger != 0)
        scheduler->triggerEvent(stopTrigger, this);

    if (m_rtspThread.joinable())
        m_rtspThread.join();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_running.store(false);
    m_startOk = false;
}

bool Live555RtspServer::pushAnnexBFrame(const std::string& streamName,
                                        const uint8_t* data,
                                        size_t size,
                                        uint64_t timestampUs)
{
    const PublishStreamPtr stream = findStream(streamName);
    if (stream == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(stream->clientStateMutex);
    if (stream->activeClientCount == 0)
        return true;

    stream->frameQueue->pushAnnexBFrame(data, size, timestampUs);
    return true;
}

bool Live555RtspServer::pushAacAccessUnit(EncodedAudioPacketPtr packet)
{
    if (!packet || packet->codec != AUDIO_CODEC_AAC || packet->bytes.empty()) {
        return false;
    }

    /*
     * m_aacPublishStreams 只会在 start() 前写入。这里持有 Server 锁读取它，防止调用方
     * 在尚未启动的边界错误地并发 addStream()；不再为每个 AAC 包复制 vector 快照。
     * 锁顺序始终是 m_mutex -> clientStateMutex，与 start()/cleanup 路径一致。
     */
    std::lock_guard<std::mutex> serverLock(m_mutex);
    const bool hasAacTrack = !m_aacPublishStreams.empty();
    for (const PublishStreamPtr& stream : m_aacPublishStreams) {
        std::lock_guard<std::mutex> lock(stream->clientStateMutex);
        if (stream->audioQueue == nullptr) {
            continue;
        }
        if (stream->activeAudioClientCount == 0) {
            continue;
        }

        // 这里只复制 shared_ptr；main/sub 的 queue 共同引用 AudioPipeline 编出的同一份 AAC payload。
        const bool dropped = stream->audioQueue->push(packet);
        if (dropped) {
            const uint64_t droppedCount = stream->audioQueue->droppedAccessUnits();
            if (droppedCount == 1 || droppedCount % 100 == 0) {
                LOG_WARN("Live555RtspServer", "stream=" << stream->config.streamName
                                                            << " AAC queue 已淘汰旧包累计=" << droppedCount);
            }
        }
    }
    return hasAacTrack;
}

bool Live555RtspServer::isRunning() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_running.load() && m_startOk;
}

bool Live555RtspServer::hasActiveClients(const std::string& streamName) const
{
    const PublishStreamPtr stream = findStream(streamName);
    if (stream == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(stream->clientStateMutex);
    return stream->activeClientCount != 0;
}

std::string Live555RtspServer::rtspURL(const std::string& streamName) const
{
    const PublishStreamPtr stream = findStream(streamName);
    if (stream == nullptr)
        return {};

    std::lock_guard<std::mutex> lock(stream->clientStateMutex);
    return stream->url;
}

std::string Live555RtspServer::lastError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

void Live555RtspServer::rtspThreadMain()
{
    const bool setupOk = setupLive555();
    if (!setupOk) {
        cleanupLive555();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_startOk = false;
            m_startFinished = true;
            m_running.store(false);
        }
        m_startCv.notify_one();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_startOk = true;
        m_startFinished = true;
    }
    m_startCv.notify_one();

    m_env->taskScheduler().doEventLoop(&m_eventLoopWatchVariable);
    cleanupLive555();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_running.store(false);
}

bool Live555RtspServer::setupLive555()
{
    m_scheduler = BasicTaskScheduler::createNew();
    if (m_scheduler == nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setErrorLocked("创建 live555 TaskScheduler 失败");
        return false;
    }

    m_env = BasicUsageEnvironment::createNew(*m_scheduler);
    if (m_env == nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setErrorLocked("创建 live555 UsageEnvironment 失败");
        return false;
    }

    m_stopTrigger = m_scheduler->createEventTrigger(stopEventCallback);

    if (!m_username.empty() || !m_password.empty()) {
        m_authDb = new UserAuthenticationDatabase;
        m_authDb->addUserRecord(m_username.c_str(), m_password.c_str());
    }

    m_server = RTSPServer::createNew(*m_env, m_rtspPort, m_authDb, kRtspClientReclamationSeconds);
    if (m_server == nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setErrorLocked(std::string("创建 RTSP server 失败: ") + m_env->getResultMsg());
        return false;
    }

    // 一个 RTSPServer 注册多个 ServerMediaSession；session 名就是各自的 URL path。
    for (const auto& entry : m_streamMap) {
        const PublishStreamPtr& stream = entry.second;
        const StreamConfig& config = stream->config;
        ServerMediaSession* session = ServerMediaSession::createNew(*m_env,
                                                                      config.streamName.c_str(),
                                                                      config.streamName.c_str(),
                                                                      "MultiCamRenderer live video stream");
        if (session == nullptr) {
            std::lock_guard<std::mutex> lock(m_mutex);
            setErrorLocked("创建 ServerMediaSession 失败: " + config.streamName);
            return false;
        }

        session->addSubsession(VideoSubsession::createNew(
            *m_env, stream->frameQueue, config.codec,
            [this, stream](bool started) { onClientPlaybackStateChanged(stream, started); }));
        if (config.audio.enabled) {
            session->addSubsession(AacAudioSubsession::createNew(
                *m_env, stream->audioQueue, config.audio.encoded.sourceFormat,
                config.audio.encoded.bitrate / 1000,
                [this, stream](bool started) { onAudioClientPlaybackStateChanged(stream, started); }));
        }
        m_server->addServerMediaSession(session);

        char* url = m_server->rtspURL(session);
        bool hasUrl = false;
        {
            std::lock_guard<std::mutex> clientLock(stream->clientStateMutex);
            stream->url = url != nullptr ? url : "";
            hasUrl = !stream->url.empty();
        }
        delete[] url;

        if (!hasUrl) {
            std::lock_guard<std::mutex> lock(m_mutex);
            setErrorLocked("生成 RTSP URL 失败: " + config.streamName);
            return false;
        }

        LOG_INFO("Live555RtspServer", "注册 RTSP stream=" << config.streamName
                                                               << " url=" << rtspURL(config.streamName)
                                                               << " video=" << codecName(config.codec)
                                                               << (config.audio.enabled
                                                                       ? " audio=AAC-LC/"
                                                                           + std::to_string(config.audio.encoded.sourceFormat.sampleRate)
                                                                           + "Hz/"
                                                                           + std::to_string(config.audio.encoded.sourceFormat.channels)
                                                                           + "ch"
                                                                       : ""));
    }
    return true;
}

void Live555RtspServer::cleanupLive555()
{
    if (m_scheduler != nullptr && m_stopTrigger != 0) {
        m_scheduler->deleteEventTrigger(m_stopTrigger);
        m_stopTrigger = 0;
    }

    if (m_server != nullptr) {
        Medium::close(m_server);
        m_server = nullptr;
    }

    delete m_authDb;
    m_authDb = nullptr;

    if (m_env != nullptr) {
        m_env->reclaim();
        m_env = nullptr;
    }

    delete m_scheduler;
    m_scheduler = nullptr;

    std::vector<std::shared_ptr<RtspPublishSink>> sinksToStop;
    std::vector<std::shared_ptr<RtspAudioPublishSink>> audioSinksToStop;
    for (const auto& entry : m_streamMap) {
        const PublishStreamPtr& stream = entry.second;
        bool hadActiveClient = false;
        bool hadActiveAudioClient = false;
        {
            std::lock_guard<std::mutex> clientLock(stream->clientStateMutex);
            hadActiveClient = stream->activeClientCount != 0;
            hadActiveAudioClient = stream->activeAudioClientCount != 0;
            stream->activeClientCount = 0;
            stream->activeAudioClientCount = 0;
            stream->frameQueue->clear();
            if (stream->audioQueue != nullptr) {
                stream->audioQueue->clear();
            }
        }
        if (hadActiveClient) {
            if (const std::shared_ptr<RtspPublishSink> sink = stream->publishSink.lock())
                sinksToStop.push_back(sink);
        }
        if (hadActiveAudioClient) {
            if (const std::shared_ptr<RtspAudioPublishSink> sink = stream->audioPublishSink.lock())
                audioSinksToStop.push_back(sink);
        }
    }

    // Server 停止时 live555 未必会逐个回调 deleteStream()；统一补发最后客户端离开事件，
    // 确保编码 worker 必定停下来。不能持有 clientStateMutex 调用外部代码。
    for (const std::shared_ptr<RtspPublishSink>& sink : sinksToStop)
        sink->onClientPlaybackEvent(RtspClientPlaybackEvent::LastClientStopped);
    for (const std::shared_ptr<RtspAudioPublishSink>& sink : audioSinksToStop)
        sink->setStreamActive(false);
}

void Live555RtspServer::onAudioClientPlaybackStateChanged(const PublishStreamPtr& stream, bool started)
{
    bool shouldNotifySink = false;
    bool sinkActive = false;
    unsigned activeAudioClientCount = 0;
    {
        std::lock_guard<std::mutex> lock(stream->clientStateMutex);
        if (stream->audioQueue == nullptr) {
            return;
        }
        if (started) {
            ++stream->activeAudioClientCount;
            shouldNotifySink = stream->activeAudioClientCount == 1;
            sinkActive = true;
        } else {
            if (stream->activeAudioClientCount == 0) {
                return;
            }
            --stream->activeAudioClientCount;
            if (stream->activeAudioClientCount == 0) {
                stream->audioQueue->clear();
                shouldNotifySink = true;
            }
        }
        activeAudioClientCount = stream->activeAudioClientCount;
    }

    LOG_INFO("Live555RtspServer", "stream=" << stream->config.streamName
                                                 << " active audio clients=" << activeAudioClientCount
                                                 << (!started && activeAudioClientCount == 0 ? " (queue cleared)" : ""));

    // 与视频一样，不持有 clientStateMutex 调用外部 Sink；Sink 只投递 worker 控制命令。
    if (shouldNotifySink) {
        if (const std::shared_ptr<RtspAudioPublishSink> sink = stream->audioPublishSink.lock()) {
            sink->setStreamActive(sinkActive);
        }
    }
}

void Live555RtspServer::onClientPlaybackStateChanged(const PublishStreamPtr& stream, bool started)
{
    bool shouldNotifySink = false;
    RtspClientPlaybackEvent playbackEvent = RtspClientPlaybackEvent::FirstClientStarted;
    unsigned activeClientCount = 0;
    {
        std::lock_guard<std::mutex> lock(stream->clientStateMutex);
        if (started) {
            ++stream->activeClientCount;
            shouldNotifySink = true;
            playbackEvent = stream->activeClientCount == 1
                ? RtspClientPlaybackEvent::FirstClientStarted
                : RtspClientPlaybackEvent::AdditionalClientStarted;
        } else {
            if (stream->activeClientCount == 0)
                return;

            --stream->activeClientCount;
            if (stream->activeClientCount == 0) {
                stream->frameQueue->clear();
                shouldNotifySink = true;
                playbackEvent = RtspClientPlaybackEvent::LastClientStopped;
            }
        }
        activeClientCount = stream->activeClientCount;
    }

    LOG_INFO("Live555RtspServer", "stream=" << stream->config.streamName
                                                 << " active clients=" << activeClientCount
                                                 << (!started && activeClientCount == 0 ? " (queue cleared)" : ""));

    // 不能持有 clientStateMutex 调用外部代码。Sink 只置 worker 标志，MPP 操作仍由
    // PublishSink 的编码 worker 串行执行。
    if (shouldNotifySink) {
        if (const std::shared_ptr<RtspPublishSink> sink = stream->publishSink.lock())
            sink->onClientPlaybackEvent(playbackEvent);
    }
}

Live555RtspServer::PublishStreamPtr Live555RtspServer::findStream(const std::string& streamName) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_streamMap.find(streamName);
    return it == m_streamMap.end() ? nullptr : it->second;
}

void Live555RtspServer::setErrorLocked(const std::string& message)
{
    m_lastError = message;
}

void Live555RtspServer::stopEventCallback(void* clientData)
{
    (void)clientData;
}
