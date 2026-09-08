#include "Live555RtspClient.hh"

#include "AnnexBSink.hh"

#include <BasicUsageEnvironment.hh>
#include <liveMedia.hh>

#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

namespace {

constexpr unsigned kMaxReceivedNaluBytes = 2U * 1024U * 1024U;

bool codecFromSubsession(const MediaSubsession& subsession, VideoCodec& codec)
{
    if (std::strcmp(subsession.mediumName(), "video") != 0) {
        return false;
    }
    if (std::strcmp(subsession.codecName(), "H264") == 0) {
        codec = VideoCodec::H264;
        return true;
    }
    if (std::strcmp(subsession.codecName(), "H265") == 0) {
        codec = VideoCodec::H265;
        return true;
    }
    return false;
}

} // namespace

struct Live555RtspClient::Impl {
    struct PullClientState {
        ~PullClientState()
        {
            delete iterator;
            if (session != nullptr) {
                Medium::close(session);
            }
        }

        MediaSubsessionIterator* iterator = nullptr;
        MediaSession* session = nullptr;
        MediaSubsession* subsession = nullptr;
        VideoCodec selectedCodec = VideoCodec::H264;
        bool hasSelectedVideo = false;
    };

    class Client final : public RTSPClient {
    public:
        static Client* createNew(UsageEnvironment& env, const char* url, Impl& owner)
        {
            return new Client(env, url, owner);
        }

        // RTSPClient 的异步回调只给出 RTSPClient*；通过 owner 回到本 Impl。
        PullClientState state;
        Impl& owner;

    private:
        Client(UsageEnvironment& env, const char* url, Impl& owner)
            // env：live555 的事件/日志环境；url：待连接的 RTSP 地址。
            // 1：日志详细度；0：不走 HTTP tunnel；-1：由 live555 创建 RTSP TCP socket。
            : RTSPClient(env, url, 1, "Live555RtspClient", 0, -1), owner(owner)
        {
        }
        ~Client() override = default;
    };

    bool start(const std::string& url,
               AnnexBNaluCallback naluCallback,
               bool requestRtpOverTcp,
               StateCallback stateCallback)
    {
        if (url.empty() || !naluCallback) {
            const std::string message = "RTSP URL 或 NALU 回调为空";
            setError(message);
            if (stateCallback) {
                stateCallback(State::Error, message);
            }
            return false;
        }

        stop();
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            url_ = url;
            naluCallback_ = std::move(naluCallback);
            stateCallback_ = std::move(stateCallback);
            requestRtpOverTcp_ = requestRtpOverTcp;
            codec_.store(VideoCodec::H264);
            setEventLoopWatchValue(0);
            stopRequestedByCaller.store(false);
            terminalStateReported.store(false);
            running.store(true);
        }
        clearError();
        notifyState(State::Connecting, {});
        eventThread = std::thread(&Impl::eventThreadMain, this);
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            stopRequestedByCaller.store(true);
            setEventLoopWatchValue(1);
            if (scheduler != nullptr && stopTrigger != 0) {
                scheduler->triggerEvent(stopTrigger, this);
            }
        }

        // 回调中调用 stop() 时不能 join 自己；回调返回后事件循环会自然退出。
        if (eventThread.joinable() && eventThread.get_id() != std::this_thread::get_id()) {
            eventThread.join();
        }
    }

    bool isRunning() const
    {
        return running.load();
    }

    VideoCodec codec() const
    {
        return codec_.load();
    }

    std::string lastError() const
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        return lastError_;
    }

    void eventThreadMain()
    {
        if (setupLive555()) {
            // 阻塞处理 RTSP 控制响应、RTP/RTCP socket 事件和 live555 定时任务。
            // watch 变量变为非 0 时退出循环，随后统一释放 live555 对象。
            env->taskScheduler().doEventLoop(&eventLoopWatchVariable);
        }
        cleanupLive555();
        running.store(false);

        if (stopRequestedByCaller.load()) {
            notifyState(State::Stopped, {});
            return;
        }

        // 异步失败路径通常已通过 reportError() 上报；这里覆盖没有明确错误信息的异常退出。
        if (!terminalStateReported.exchange(true)) {
            std::string message = lastError();
            if (message.empty()) {
                message = "RTSP 事件循环异常退出";
                setError(message);
            }
            notifyState(State::Error, message);
        }
    }

    bool setupLive555()
    {
        std::string error;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            scheduler = BasicTaskScheduler::createNew();
            if (scheduler == nullptr) {
                error = "创建 TaskScheduler 失败";
            } else {
                env = BasicUsageEnvironment::createNew(*scheduler);
                if (env == nullptr) {
                    error = "创建 UsageEnvironment 失败";
                } else {
                    // stop() 可从其他线程调用；triggerEvent() 用它唤醒阻塞的 doEventLoop()。
                    stopTrigger = scheduler->createEventTrigger(stopEventCallback);

                    // *env 是 live555 环境；url_.c_str() 是 RTSP URL；*this 用于异步回调回到 Impl。
                    client = Client::createNew(*env, url_.c_str(), *this);
                    if (client == nullptr) {
                        error = std::string("创建 RTSPClient 失败：") + env->getResultMsg();
                    }
                }
            }
        }

        // 状态回调可能回到 Manager，不能在 lifecycleMutex 持有期间触发。
        if (!error.empty()) {
            reportError(error);
            return false;
        }
        // 异步请求 SDP；响应到达后由 continueAfterDESCRIBE() 接续处理。
        client->sendDescribeCommand(continueAfterDESCRIBE);
        return true;
    }

    void cleanupLive555()
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex);
        if (client != nullptr) {
            const bool sessionWasActive = closeSinks(*client);
            if (sessionWasActive && client->state.session != nullptr) {
                // 主动停止时通知服务端，服务端可立即回收该播放会话。
                client->sendTeardownCommand(*client->state.session, nullptr);
            }
            Medium::close(client);
            client = nullptr;
        }
        if (scheduler != nullptr && stopTrigger != 0) {
            scheduler->deleteEventTrigger(stopTrigger);
            stopTrigger = 0;
        }
        if (env != nullptr) {
            env->reclaim();
            env = nullptr;
        }
        delete scheduler;
        scheduler = nullptr;
    }

    static void stopEventCallback(void* clientData)
    {
        // 此回调不做业务：它的唯一职责是把 TaskScheduler 从阻塞等待中唤醒。
        // 真正的退出条件是 eventLoopWatchVariable 已被 stop() 置为 1。
        (void)clientData;
    }

    static void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString)
    {
        // live555 使用静态 C 风格回调；Client 保存 owner，因此可转回 Impl 成员函数。
        auto* client = static_cast<Client*>(rtspClient);
        client->owner.continueAfterDESCRIBE(*client, resultCode, resultString);
    }

    static void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString)
    {
        auto* client = static_cast<Client*>(rtspClient);
        client->owner.continueAfterSETUP(*client, resultCode, resultString);
    }

    static void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString)
    {
        auto* client = static_cast<Client*>(rtspClient);
        client->owner.continueAfterPLAY(*client, resultCode, resultString);
    }

    static void afterSubsessionPlaying(void* clientData)
    {
        auto* client = static_cast<Client*>(clientData);
        client->owner.reportError("RTP 视频源已关闭");
        client->owner.requestEventLoopExit();
    }

    void continueAfterDESCRIBE(Client& clientRef, int resultCode, char* resultString)
    {
        std::unique_ptr<char[]> response(resultString);
        if (resultCode != 0) {
            reportError(std::string("DESCRIBE 失败：") + (resultString == nullptr ? "" : resultString));
            requestEventLoopExit();
            return;
        }

        // resultString 是 SDP；MediaSession 解析出其中的 MediaSubsession（各个 track）。
        clientRef.state.session = MediaSession::createNew(clientRef.envir(), resultString);
        if (clientRef.state.session == nullptr || !clientRef.state.session->hasSubsessions()) {
            reportError(std::string("SDP 没有可用媒体轨道：") + clientRef.envir().getResultMsg());
            requestEventLoopExit();
            return;
        }

        clientRef.state.iterator = new MediaSubsessionIterator(*clientRef.state.session);
        setupNextSubsession(clientRef);
    }

    void continueAfterSETUP(Client& clientRef, int resultCode, char* resultString)
    {
        std::unique_ptr<char[]> response(resultString);
        if (resultCode != 0) {
            reportError(std::string("SETUP 失败：") + (resultString == nullptr ? "" : resultString));
            requestEventLoopExit();
            return;
        }

        MediaSubsession& subsession = *clientRef.state.subsession;
        // readSource() 产生已完成 RTP 解包/分片重组的帧；AnnexBSink 负责补 Annex-B 起始码。
        subsession.sink = AnnexBSink::createNew(
            clientRef.envir(),
            clientRef.state.selectedCodec,
            naluCallback_,
            [this](const std::string& message) {
                reportError(message);
                requestEventLoopExit();
            },
            kMaxReceivedNaluBytes);
        if (subsession.sink == nullptr) {
            reportError("创建 AnnexBSink 失败");
            requestEventLoopExit();
            return;
        }

        // 先让 Sink 登记 getNextFrame() 回调，再发送 PLAY，避免首个 RTP 包没有接收者。
        subsession.sink->startPlaying(*subsession.readSource(), afterSubsessionPlaying, &clientRef);
        setupNextSubsession(clientRef);
    }

    void continueAfterPLAY(Client& /*clientRef*/, int resultCode, char* resultString)
    {
        std::unique_ptr<char[]> response(resultString);
        if (resultCode != 0) {
            reportError(std::string("PLAY 失败：") + (resultString == nullptr ? "" : resultString));
            requestEventLoopExit();
            return;
        }
        notifyState(State::Playing, {});
    }

    void setupNextSubsession(Client& clientRef)
    {
        while ((clientRef.state.subsession = clientRef.state.iterator->next()) != nullptr) {
            MediaSubsession& subsession = *clientRef.state.subsession;
            VideoCodec codec;
            if (!codecFromSubsession(subsession, codec)) {
                continue;
            }
            if (clientRef.state.hasSelectedVideo) {
                // 一个 RtspStream 对应一路视频；多视频 track 应创建多个客户端实例。
                continue;
            }
            // 按 SDP 在本地创建 RTP/RTCP 接收链路；UDP 模式下也会在此申请本地接收端口。
            // 这一步尚未通知服务端，服务端侧传输会话由后面的 SETUP 建立。
            if (!subsession.initiate()) {
                reportError(std::string("初始化 RTP 接收端失败：") + clientRef.envir().getResultMsg());
                requestEventLoopExit();
                return;
            }

            clientRef.state.selectedCodec = codec;
            clientRef.state.hasSelectedVideo = true;
            codec_.store(codec);

            // 请求服务端为该 track 建立传输会话：
            // UDP 时携带本地 RTP/RTCP 端口；TCP 时请求 RTP over RTSP interleaved。
            clientRef.sendSetupCommand(subsession, continueAfterSETUP, False, requestRtpOverTcp_ ? True : False);
            return;
        }

        if (!clientRef.state.hasSelectedVideo) {
            reportError("RTSP 流中没有 H264/H265 视频轨道");
            requestEventLoopExit();
            return;
        }
        // Sink 已先登记 getNextFrame()；现在通知服务端正式开始发送 RTP。
        clientRef.sendPlayCommand(*clientRef.state.session, continueAfterPLAY);
    }

    static bool closeSinks(Client& clientRef)
    {
        if (clientRef.state.session == nullptr) {
            return false;
        }
        bool sessionWasActive = false;
        MediaSubsessionIterator iterator(*clientRef.state.session);
        MediaSubsession* subsession = nullptr;
        while ((subsession = iterator.next()) != nullptr) {
            if (subsession->sink != nullptr) {
                Medium::close(subsession->sink);
                subsession->sink = nullptr;
                sessionWasActive = true;
            }
        }
        return sessionWasActive;
    }

    void requestEventLoopExit()
    {
        // 在事件线程内设置退出条件即可；外部线程 stop() 还会额外 triggerEvent() 唤醒循环。
        setEventLoopWatchValue(1);
    }

    void notifyState(State state, const std::string& message)
    {
        StateCallback callback;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex);
            callback = stateCallback_;
        }
        if (callback) {
            callback(state, message);
        }
    }

    void reportError(const std::string& message)
    {
        setError(message);
        terminalStateReported.store(true);
        notifyState(State::Error, message);
    }

    void setEventLoopWatchValue(char value)
    {
#if LIVEMEDIA_LIBRARY_VERSION_INT >= 1700000000
        eventLoopWatchVariable.store(value);
#else
        eventLoopWatchVariable = value;
#endif
    }

    void setError(const std::string& message)
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastError_ = message;
    }

    void clearError()
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastError_.clear();
    }

    mutable std::mutex lifecycleMutex;
    mutable std::mutex errorMutex;
    std::thread eventThread;
    std::atomic_bool running {false};
    std::atomic_bool stopRequestedByCaller {false};
    std::atomic_bool terminalStateReported {false};
    std::atomic<VideoCodec> codec_ {VideoCodec::H264};
#if LIVEMEDIA_LIBRARY_VERSION_INT >= 1700000000
    EventLoopWatchVariable eventLoopWatchVariable {0};
#else
    char volatile eventLoopWatchVariable = 0;
#endif

    TaskScheduler* scheduler = nullptr;
    UsageEnvironment* env = nullptr;
    Client* client = nullptr;
    EventTriggerId stopTrigger = 0;

    std::string url_;
    AnnexBNaluCallback naluCallback_;
    StateCallback stateCallback_;
    bool requestRtpOverTcp_ = false;
    std::string lastError_;
};

Live555RtspClient::Live555RtspClient()
    : impl_(std::make_unique<Impl>())
{
}

Live555RtspClient::~Live555RtspClient()
{
    stop();
}

bool Live555RtspClient::start(const std::string& url,
                              AnnexBNaluCallback naluCallback,
                              bool requestRtpOverTcp,
                              StateCallback stateCallback)
{
    return impl_->start(url,
                        std::move(naluCallback),
                        requestRtpOverTcp,
                        std::move(stateCallback));
}

void Live555RtspClient::stop()
{
    impl_->stop();
}

bool Live555RtspClient::isRunning() const
{
    return impl_->isRunning();
}

VideoCodec Live555RtspClient::codec() const
{
    return impl_->codec();
}

std::string Live555RtspClient::lastError() const
{
    return impl_->lastError();
}
