#include "Live555RtspServer.hh"

#include "AnnexBFrameQueue.hh"
#include "Log.hpp"
#include "VideoSubsession.hh"

#include <utility>

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

} // namespace

Live555RtspServer::RtspPublishStream::RtspPublishStream(StreamConfig streamConfig)
    : config(std::move(streamConfig)),
      frameQueue(std::make_shared<AnnexBFrameQueue>())
{
}

Live555RtspServer::Live555RtspServer() = default;

Live555RtspServer::~Live555RtspServer()
{
    stop();
}

bool Live555RtspServer::addStream(const StreamConfig& config)
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

    m_streamMap.emplace(config.streamName, std::make_shared<RtspPublishStream>(config));
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

    m_rtspPort = rtspPort == 0 ? 8554 : rtspPort;
    m_username = username;
    m_password = password;
    m_lastError.clear();
    m_eventLoopWatchVariable.store(0);
    m_startFinished = false;
    m_startOk = false;
    for (const auto& entry : m_streamMap) {
        const PublishStreamPtr& stream = entry.second;
        std::lock_guard<std::mutex> clientLock(stream->clientStateMutex);
        stream->activeClientCount = 0;
        stream->url.clear();
        stream->frameQueue->clear();
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

        m_eventLoopWatchVariable.store(1);
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
                                                               << " codec=" << codecName(config.codec));
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

    for (const auto& entry : m_streamMap) {
        const PublishStreamPtr& stream = entry.second;
        std::lock_guard<std::mutex> clientLock(stream->clientStateMutex);
        stream->activeClientCount = 0;
        stream->frameQueue->clear();
    }
}

void Live555RtspServer::onClientPlaybackStateChanged(const PublishStreamPtr& stream, bool started)
{
    bool activeChanged = false;
    bool active = false;
    unsigned activeClientCount = 0;
    {
        std::lock_guard<std::mutex> lock(stream->clientStateMutex);
        if (started) {
            ++stream->activeClientCount;
            activeChanged = stream->activeClientCount == 1;
            active = true;
        } else {
            if (stream->activeClientCount == 0)
                return;

            --stream->activeClientCount;
            activeChanged = stream->activeClientCount == 0;
            active = false;
            if (activeChanged)
                stream->frameQueue->clear();
        }
        activeClientCount = stream->activeClientCount;
    }

    LOG_INFO("Live555RtspServer", "stream=" << stream->config.streamName
                                                 << " active clients=" << activeClientCount
                                                 << (!started && activeClientCount == 0 ? " (queue cleared)" : ""));

    // 不能持有 clientStateMutex 调用外部代码；IpcApp 会在该回调中投递相机/编码器启停命令。
    if (activeChanged && stream->config.onClientActiveChanged)
        stream->config.onClientActiveChanged(active);
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
