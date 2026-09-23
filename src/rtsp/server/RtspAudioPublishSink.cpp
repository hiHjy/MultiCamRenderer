#include "RtspAudioPublishSink.hpp"

#include "Live555RtspServer.hh"
#include "Log.hpp"

#include <utility>

RtspAudioPublishSink::RtspAudioPublishSink(AudioPipeline& pipeline,
                                           Live555RtspServer& server,
                                           Config config)
    : m_pipeline(pipeline)
    , m_server(server)
    , m_config(config)
    , m_dispatchState(std::make_shared<DispatchState>(server))
{
    if (!m_pipeline.getEncodedStreamInfo(m_config.codec, m_config.bitratePreset, m_streamInfo)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setErrorLocked("获取 RTSP 音频流描述失败: " + m_pipeline.lastError());
        return;
    }
    m_workerThread = std::thread(&RtspAudioPublishSink::workerMain, this);
}

RtspAudioPublishSink::~RtspAudioPublishSink()
{
    m_dispatchState->acceptingPackets.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_activeStreamCount = 0;
        m_encodingRequested = false;
        m_stopping = true;
    }
    m_cv.notify_one();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void RtspAudioPublishSink::setStreamActive(bool active)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            return;
        }

        if (active) {
            ++m_activeStreamCount;
            if (m_activeStreamCount == 1) {
                m_encodingRequested = true;
                m_activationAttempted = false;
                changed = true;
            }
        } else {
            if (m_activeStreamCount == 0) {
                return;
            }
            --m_activeStreamCount;
            if (m_activeStreamCount == 0) {
                m_encodingRequested = false;
                changed = true;
            }
        }
    }
    if (changed) {
        m_cv.notify_one();
    }
}

AudioEncodedStreamInfo RtspAudioPublishSink::streamInfo() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_streamInfo;
}

bool RtspAudioPublishSink::isReady() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_stopping && m_lastError.empty() && m_streamInfo.codec != AUDIO_CODEC_UNKNOWN;
}

bool RtspAudioPublishSink::isEncoding() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_encoding;
}

std::string RtspAudioPublishSink::lastError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

void RtspAudioPublishSink::workerMain()
{
    AudioSubscription subscription;
    bool subscribed = false;

    while (true) {
        bool shouldStart = false;
        bool shouldStop = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this, &subscribed] {
                return m_stopping
                    || (m_encodingRequested && !subscribed && !m_activationAttempted)
                    || (!m_encodingRequested && subscribed);
            });
            if (m_stopping) {
                break;
            }

            shouldStart = m_encodingRequested && !subscribed && !m_activationAttempted;
            shouldStop = !m_encodingRequested && subscribed;
            if (shouldStart) {
                /* 同一个“全局首客户端”边界只尝试一次；失败等待下一次 0->1 才重试。 */
                m_activationAttempted = true;
            }
        }

        if (shouldStop) {
            /* 先拒绝晚到的 Hub 快照回调，再释放 subscription；callback 不捕获 this，析构安全。 */
            m_dispatchState->acceptingPackets.store(false, std::memory_order_release);
            subscription.reset();
            subscribed = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_encoding = false;
                m_activationAttempted = false;
            }
            LOG_INFO("RtspAudioPublishSink", "最后一个 RTSP 音频客户端离开，已停止音频编码");
            continue;
        }

        if (!shouldStart) {
            continue;
        }

        const std::shared_ptr<DispatchState> dispatchState = m_dispatchState;
        AudioSubscription created = m_pipeline.subscribeEncoded(
            m_config.codec, m_config.bitratePreset,
            [dispatchState](EncodedAudioPacketPtr packet) {
                if (!dispatchState->acceptingPackets.load(std::memory_order_acquire)) {
                    return;
                }
                dispatchState->server.pushAacAccessUnit(std::move(packet));
            });

        bool keepSubscription = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!created.valid()) {
                setErrorLocked("创建 RTSP 音频编码订阅失败: " + m_pipeline.lastError());
            } else if (!m_stopping && m_encodingRequested) {
                m_encoding = true;
                m_lastError.clear();
                keepSubscription = true;
            }
        }

        if (!keepSubscription) {
            created.reset();
            continue;
        }

        subscription = std::move(created);
        subscribed = true;
        m_dispatchState->acceptingPackets.store(true, std::memory_order_release);
        LOG_INFO("RtspAudioPublishSink", "首个 RTSP 音频客户端到达，已启动 "
                                               << (m_config.codec == AUDIO_CODEC_AAC ? "AAC-LC" : "Opus")
                                               << " 编码 bitrate=" << m_streamInfo.bitrate);
    }

    m_dispatchState->acceptingPackets.store(false, std::memory_order_release);
    subscription.reset();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_encoding = false;
}

void RtspAudioPublishSink::setErrorLocked(const std::string& message)
{
    m_lastError = message;
    LOG_ERROR("RtspAudioPublishSink", message);
}
