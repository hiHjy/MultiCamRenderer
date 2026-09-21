#include "AudioPipeline.hpp"

#include "Log.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <thread>
#include <utility>

namespace {

bool samePcmFormat(const AudioPcmFormat& left, const AudioPcmFormat& right)
{
    return left.sampleRate == right.sampleRate && left.channels == right.channels
        && left.sampleFormat == right.sampleFormat;
}

bool sameApmConfig(const AudioApmConfig& left, const AudioApmConfig& right)
{
    return left.enableAudioProcessing == right.enableAudioProcessing
        && left.enableEchoCancellation == right.enableEchoCancellation
        && left.aecStreamDelayMs == right.aecStreamDelayMs
        && left.enableAdaptiveDigitalGain == right.enableAdaptiveDigitalGain
        && left.maxGainDb == right.maxGainDb && left.initialGainDb == right.initialGainDb
        && left.maxGainChangeDbPerSecond == right.maxGainChangeDbPerSecond
        && left.headroomDb == right.headroomDb
        && left.maxOutputNoiseLevelDbfs == right.maxOutputNoiseLevelDbfs
        && left.fixedDigitalGainDb == right.fixedDigitalGainDb
        && left.enableNoiseSuppression == right.enableNoiseSuppression
        && left.noiseSuppressionLevel == right.noiseSuppressionLevel
        && left.enableHighPassFilter == right.enableHighPassFilter
        && left.enableTransientSuppression == right.enableTransientSuppression;
}

bool sameEncoderConfig(const AudioEncoderConfig& left, const AudioEncoderConfig& right)
{
    return left.codec == right.codec && left.bitrate == right.bitrate
        && left.frameDurationUs == right.frameDurationUs
        && left.maxPacketBytes == right.maxPacketBytes;
}

uint64_t frameDurationUs(const AudioFrame& frame)
{
    if (frame.format.sampleRate == 0) {
        return 0;
    }
    return static_cast<uint64_t>(frame.frames) * 1000000ULL / frame.format.sampleRate;
}

size_t pcmBufferBytes(const AudioPcmFormat& format, size_t frames)
{
    return frames * format.channels * sizeof(int16_t);
}

bool isTimestampDiscontinuous(uint64_t expectedTimestampUs, uint64_t actualTimestampUs)
{
    /* 允许 1ms 的整数换算/调度余量；队列丢掉一个 10ms block 时会稳定越过此阈值。 */
    const uint64_t delta = expectedTimestampUs > actualTimestampUs
        ? expectedTimestampUs - actualTimestampUs
        : actualTimestampUs - expectedTimestampUs;
    return delta > 1000ULL;
}

} // namespace

AudioPipelineConfig::AudioPipelineConfig()
{
    audio_capture_config_init(&capture);
}

AudioPcmRequest::AudioPcmRequest()
{
    audio_apm_config_init(&apmConfig);
}

AudioEncodedRequest::AudioEncodedRequest()
{
    audio_apm_config_init(&apmConfig);
    audio_encoder_config_init(&encoderConfig);
}

AudioSubscription::AudioSubscription(std::function<void()> unsubscribe, std::shared_ptr<void> keepAlive)
    : m_unsubscribe(std::move(unsubscribe))
    , m_keepAlive(std::move(keepAlive))
{
}

AudioSubscription::~AudioSubscription()
{
    reset();
}

void AudioSubscription::reset()
{
    if (m_unsubscribe) {
        m_unsubscribe();
        m_unsubscribe = {};
    }
    m_keepAlive.reset();
}

bool AudioSubscription::valid() const
{
    return static_cast<bool>(m_unsubscribe);
}

class AudioPipeline::ApmNode final : public std::enable_shared_from_this<AudioPipeline::ApmNode> {
public:
    ApmNode(AudioApmConfig config,
            uint32_t warmupDiscardDurationMs,
            size_t queueCapacity,
            size_t framePoolCapacity)
        : m_config(config)
        , m_warmupDiscardDurationMs(warmupDiscardDurationMs)
        , m_queueCapacity(std::max<size_t>(1, queueCapacity))
        , m_framePoolCapacity(std::max<size_t>(1, framePoolCapacity))
        , m_outputHub(std::make_shared<AudioPcmHub>())
    {
        /* 创建 APM Node 就表示这一支路明确需要处理，不允许悄悄退化成直通。 */
        m_config.enableAudioProcessing = 1;
    }

    ~ApmNode()
    {
        stop();
        m_inputSubscription.reset();
    }

    bool matches(const AudioPcmRequest& request) const
    {
        AudioApmConfig requested = request.apmConfig;
        requested.enableAudioProcessing = 1;
        return sameApmConfig(m_config, requested)
            && m_warmupDiscardDurationMs == request.apmWarmupDiscardDurationMs;
    }

    void attachInput(const std::shared_ptr<AudioPcmHub>& inputHub)
    {
        std::weak_ptr<ApmNode> weakSelf = shared_from_this();
        m_inputSubscription = inputHub->subscribe([weakSelf](AudioFramePtr frame) {
            if (const std::shared_ptr<ApmNode> self = weakSelf.lock()) {
                self->enqueue(std::move(frame));
            }
        });
    }

    bool start(const AudioPcmFormat& format, size_t maximumInputFrames)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) {
            return true;
        }
        if (format.sampleRate == 0 || format.channels == 0 || format.sampleRate % 100 != 0
            || maximumInputFrames == 0) {
            m_lastError = "APM 需要有效且可按 10ms 对齐的 PCM 格式";
            return false;
        }

        const int result = audio_apm_open(&m_apm, &m_config, format, format.sampleRate / 100);
        if (result < 0) {
            m_lastError = "audio_apm_open 失败: " + std::to_string(result);
            return false;
        }

        m_format = format;
        m_outputFramePool = std::make_shared<AudioFramePool>(
            m_framePoolCapacity, pcmBufferBytes(format, maximumInputFrames));
        m_droppedOutputFramesByPool = 0;
        m_warmupFramesRemaining = (static_cast<uint64_t>(m_warmupDiscardDurationMs) * format.sampleRate + 999ULL) / 1000ULL;
        m_stopRequested = false;
        m_running = true;
        m_worker = std::thread(&ApmNode::workerMain, this);
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) {
                return;
            }
            m_stopRequested = true;
            m_queue.clear();
            m_cv.notify_all();
        }
        if (m_worker.joinable()) {
            m_worker.join();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
        m_stopRequested = false;
        audio_apm_close(&m_apm);
    }

    void enqueue(AudioFramePtr frame)
    {
        if (!frame) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running || m_stopRequested) {
            return;
        }
        if (m_queue.size() == m_queueCapacity) {
            m_queue.pop_front();
            ++m_droppedInputFrames;
        }
        m_queue.push_back(std::move(frame));
        m_cv.notify_one();
    }

    const std::shared_ptr<AudioPcmHub>& outputHub() const
    {
        return m_outputHub;
    }

    std::string lastError() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastError;
    }

private:
    void workerMain()
    {
        while (true) {
            AudioFramePtr frame;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stopRequested || !m_queue.empty(); });
                if (m_stopRequested) {
                    return;
                }
                frame = std::move(m_queue.front());
                m_queue.pop_front();
            }

            const AudioPcmFrame input = frame->pcmView();
            AudioPcmFrame processed {};
            if (!samePcmFormat(frame->format, m_format)
                || audio_apm_process_capture(&m_apm, &input, &processed) < 0) {
                LOG_WARN("ApmNode", "PCM 格式不匹配或 APM 处理失败，丢弃一帧");
                continue;
            }

            /* APM 预热时仍处理 PCM，只是不把尚未稳定的结果发布给下游。 */
            size_t skipFrames = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                skipFrames = std::min<size_t>(processed.frames, m_warmupFramesRemaining);
                m_warmupFramesRemaining -= skipFrames;
            }
            if (skipFrames == processed.frames) {
                continue;
            }
            processed.data += skipFrames * processed.format.channels * sizeof(int16_t);
            processed.frames -= skipFrames;
            processed.timestampUs += static_cast<uint64_t>(skipFrames) * 1000000ULL / processed.format.sampleRate;
            const AudioFramePtr output = m_outputFramePool->copyFrom(processed);
            if (!output) {
                ++m_droppedOutputFramesByPool;
                if (m_droppedOutputFramesByPool == 1 || m_droppedOutputFramesByPool % 100 == 0) {
                    LOG_WARN("ApmNode", "APM 输出 PCM 池已满，累计丢弃="
                                            << m_droppedOutputFramesByPool
                                            << " poolCapacity=" << m_outputFramePool->capacity());
                }
                continue;
            }
            m_outputHub->publish(output);
        }
    }

    AudioApmConfig m_config {};
    uint32_t m_warmupDiscardDurationMs = 0;
    size_t m_queueCapacity = 1;
    size_t m_framePoolCapacity = 1;
    std::shared_ptr<AudioPcmHub> m_outputHub;
    std::shared_ptr<AudioFramePool> m_outputFramePool;
    AudioPcmHub::Subscription m_inputSubscription;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<AudioFramePtr> m_queue;
    std::thread m_worker;
    AudioApm m_apm {};
    AudioPcmFormat m_format {};
    uint64_t m_warmupFramesRemaining = 0;
    uint64_t m_droppedInputFrames = 0;
    uint64_t m_droppedOutputFramesByPool = 0;
    bool m_stopRequested = false;
    bool m_running = false;
    std::string m_lastError;
};

class AudioPipeline::EncoderNode final : public std::enable_shared_from_this<AudioPipeline::EncoderNode> {
public:
    EncoderNode(std::shared_ptr<AudioPcmHub> inputHub,
                AudioEncoderConfig config,
                size_t queueCapacity,
                size_t packetPoolCapacity)
        : m_inputHub(std::move(inputHub))
        , m_config(config)
        , m_queueCapacity(std::max<size_t>(1, queueCapacity))
        , m_packetPoolCapacity(std::max<size_t>(1, packetPoolCapacity))
        , m_outputHub(std::make_shared<AudioEncodedPacketHub>())
    {
    }

    ~EncoderNode()
    {
        stop();
        m_inputSubscription.reset();
    }

    bool matches(const std::shared_ptr<AudioPcmHub>& inputHub, const AudioEncodedRequest& request) const
    {
        return m_inputHub == inputHub && sameEncoderConfig(m_config, request.encoderConfig)
            && m_queueCapacity == request.inputQueueCapacity;
    }

    void attachInput()
    {
        std::weak_ptr<EncoderNode> weakSelf = shared_from_this();
        m_inputSubscription = m_inputHub->subscribe([weakSelf](AudioFramePtr frame) {
            if (const std::shared_ptr<EncoderNode> self = weakSelf.lock()) {
                self->enqueue(std::move(frame));
            }
        });
    }

    bool start(const AudioPcmFormat& format, size_t maximumInputFrames)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) {
            return true;
        }
        if (format.sampleRate == 0 || format.channels == 0 || maximumInputFrames == 0) {
            m_lastError = "编码器输入 PCM 格式无效";
            return false;
        }

        m_packetPool = std::make_shared<EncodedAudioPacketPool>(m_packetPoolCapacity,
                                                                 m_config.maxPacketBytes);
        m_droppedOutputPacketsByPool = 0;
        audio_encoder_set_packet_callback(&m_encoder, &EncoderNode::onEncodedPacket, this);
        if (!initializeEncoderLocked(format)) {
            return false;
        }
        m_format = format;
        m_haveExpectedTimestamp = false;
        m_stopRequested = false;
        m_running = true;
        m_worker = std::thread(&EncoderNode::workerMain, this);
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) {
                return;
            }
            m_stopRequested = true;
            m_queue.clear();
            m_cv.notify_all();
        }
        if (m_worker.joinable()) {
            m_worker.join();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
        m_stopRequested = false;
        audio_encoder_close(&m_encoder);
        m_haveExpectedTimestamp = false;
    }

    void enqueue(AudioFramePtr frame)
    {
        if (!frame) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running || m_stopRequested) {
            return;
        }
        if (m_queue.size() == m_queueCapacity) {
            m_queue.pop_front();
            ++m_droppedInputFrames;
        }
        m_queue.push_back(std::move(frame));
        m_cv.notify_one();
    }

    const std::shared_ptr<AudioEncodedPacketHub>& outputHub() const
    {
        return m_outputHub;
    }

    std::string lastError() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastError;
    }

private:
    static int onEncodedPacket(const AudioEncodedPacket* packet, void* userData)
    {
        auto* self = static_cast<EncoderNode*>(userData);
        if (self == nullptr || packet == nullptr) {
            return -EINVAL;
        }
        const EncodedAudioPacketPtr copied = self->m_packetPool->copyFrom(*packet);
        if (!copied) {
            ++self->m_droppedOutputPacketsByPool;
            if (self->m_droppedOutputPacketsByPool == 1
                || self->m_droppedOutputPacketsByPool % 100 == 0) {
                LOG_WARN("EncoderNode", "编码包池已满，累计丢弃="
                                            << self->m_droppedOutputPacketsByPool
                                            << " poolCapacity=" << self->m_packetPool->capacity());
            }
            return 0;
        }
        self->m_outputHub->publish(copied);
        return 0;
    }

    bool initializeEncoderLocked(const AudioPcmFormat& format)
    {
        const int result = audio_encoder_init(&m_encoder, &m_config, &format);
        if (result < 0) {
            m_lastError = "audio_encoder_init 失败: " + std::to_string(result);
            return false;
        }
        return true;
    }

    void resetEncoderForDiscontinuity(const AudioPcmFormat& format)
    {
        /* 清掉 Opus 内部尚未凑满的一部分 PCM，绝不能跨时间缺口拼成同一个压缩包。 */
        audio_encoder_close(&m_encoder);
        audio_encoder_set_packet_callback(&m_encoder, &EncoderNode::onEncodedPacket, this);
        if (!initializeEncoderLocked(format)) {
            LOG_ERROR("EncoderNode", m_lastError);
        } else {
            LOG_WARN("EncoderNode", "检测到 PCM 时间戳不连续，已重建编码器并丢弃未完成压缩包");
        }
        m_haveExpectedTimestamp = false;
    }

    void workerMain()
    {
        while (true) {
            AudioFramePtr frame;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stopRequested || !m_queue.empty(); });
                if (m_stopRequested) {
                    return;
                }
                frame = std::move(m_queue.front());
                m_queue.pop_front();
            }

            if (!samePcmFormat(frame->format, m_format)) {
                LOG_WARN("EncoderNode", "PCM 格式变化，重建编码器");
                resetEncoderForDiscontinuity(frame->format);
                m_format = frame->format;
            } else if (m_haveExpectedTimestamp
                       && isTimestampDiscontinuous(m_expectedTimestampUs, frame->timestampUs)) {
                resetEncoderForDiscontinuity(frame->format);
            }

            const AudioPcmFrame input = frame->pcmView();
            const int result = audio_encoder_push_pcm(&m_encoder, &input);
            if (result < 0) {
                LOG_ERROR("EncoderNode", "编码 PCM 失败: " << result);
                m_haveExpectedTimestamp = false;
                continue;
            }
            m_expectedTimestampUs = frame->timestampUs + frameDurationUs(*frame);
            m_haveExpectedTimestamp = true;
        }
    }

    std::shared_ptr<AudioPcmHub> m_inputHub;
    AudioEncoderConfig m_config {};
    size_t m_queueCapacity = 1;
    size_t m_packetPoolCapacity = 1;
    std::shared_ptr<AudioEncodedPacketHub> m_outputHub;
    std::shared_ptr<EncodedAudioPacketPool> m_packetPool;
    AudioPcmHub::Subscription m_inputSubscription;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<AudioFramePtr> m_queue;
    std::thread m_worker;
    AudioEncoder m_encoder {};
    AudioPcmFormat m_format {};
    uint64_t m_expectedTimestampUs = 0;
    uint64_t m_droppedInputFrames = 0;
    uint64_t m_droppedOutputPacketsByPool = 0;
    bool m_haveExpectedTimestamp = false;
    bool m_stopRequested = false;
    bool m_running = false;
    std::string m_lastError;
};

struct AudioPipeline::KeepAlive {
    std::vector<std::shared_ptr<void>> nodes;
};

AudioPipeline::AudioPipeline(const AudioPipelineConfig& config)
    : m_config(config)
    , m_rawPcmHub(std::make_shared<AudioPcmHub>())
{
    if (m_config.defaultNodeQueueCapacity == 0) {
        m_config.defaultNodeQueueCapacity = 5;
    }
    if (m_config.pcmFramePoolCapacity == 0) {
        m_config.pcmFramePoolCapacity = 32;
    }
    if (m_config.encodedPacketPoolCapacity == 0) {
        m_config.encodedPacketPoolCapacity = 32;
    }
}

AudioPipeline::~AudioPipeline()
{
    stopCapture();
    audio_capture_destroy(m_capture);
    m_capture = nullptr;
}

bool AudioPipeline::startCapture()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_captureRunning) {
        return true;
    }

    if (m_capture == nullptr) {
        m_capture = audio_capture_create();
    }
    if (m_capture == nullptr) {
        setErrorLocked("分配 AudioCapture 失败");
        return false;
    }

    const int openResult = audio_capture_open_auto(m_capture, &m_config.capture);
    if (openResult < 0) {
        setErrorLocked("打开音频采集设备失败: " + std::to_string(openResult));
        return false;
    }

    /* ALSA 实际协商出来的格式是 Node 初始化的输入契约，必须先于 Node 启动缓存。 */
    m_captureFormat = audio_capture_actual_format(m_capture);
    m_capturePeriodFrames = audio_capture_period_frames(m_capture);
    if (m_captureFormat.sampleRate == 0 || m_captureFormat.channels == 0 || m_capturePeriodFrames == 0) {
        audio_capture_close(m_capture);
        m_captureFormat = {};
        m_capturePeriodFrames = 0;
        setErrorLocked("音频采集设备没有返回有效 PCM 格式");
        return false;
    }
    m_rawFramePool = std::make_shared<AudioFramePool>(
        m_config.pcmFramePoolCapacity, pcmBufferBytes(m_captureFormat, m_capturePeriodFrames));
    m_droppedRawFramesByPool = 0;

    const std::vector<std::shared_ptr<ApmNode>> apmNodes = collectApmNodesLocked();
    const std::vector<std::shared_ptr<EncoderNode>> encoderNodes = collectEncoderNodesLocked();
    for (const std::shared_ptr<ApmNode>& node : apmNodes) {
        if (!startNodeLocked(node)) {
            audio_capture_close(m_capture);
            m_captureFormat = {};
            m_capturePeriodFrames = 0;
            m_rawFramePool.reset();
            return false;
        }
    }
    for (const std::shared_ptr<EncoderNode>& node : encoderNodes) {
        if (!startNodeLocked(node)) {
            for (const std::shared_ptr<ApmNode>& apmNode : apmNodes) {
                apmNode->stop();
            }
            audio_capture_close(m_capture);
            m_captureFormat = {};
            m_capturePeriodFrames = 0;
            m_rawFramePool.reset();
            return false;
        }
    }

    audio_capture_set_callback(m_capture, &AudioPipeline::onCapturedPcm, this);
    const int startResult = audio_capture_start(m_capture);
    if (startResult < 0) {
        for (const std::shared_ptr<EncoderNode>& node : encoderNodes) {
            node->stop();
        }
        for (const std::shared_ptr<ApmNode>& node : apmNodes) {
            node->stop();
        }
        audio_capture_close(m_capture);
        m_captureFormat = {};
        m_capturePeriodFrames = 0;
        m_rawFramePool.reset();
        setErrorLocked("启动音频采集线程失败: " + std::to_string(startResult));
        return false;
    }

    m_captureRunning = true;
    m_lastError.clear();
    LOG_INFO("AudioPipeline", "音频采集已启动 PCM=" << m_captureFormat.sampleRate
                                                           << "Hz/" << m_captureFormat.channels
                                                           << "ch period=" << m_capturePeriodFrames);
    return true;
}

void AudioPipeline::stopCapture()
{
    std::vector<std::shared_ptr<ApmNode>> apmNodes;
    std::vector<std::shared_ptr<EncoderNode>> encoderNodes;
    bool wasRunning = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        wasRunning = m_captureRunning;
        m_captureRunning = false;
        apmNodes = collectApmNodesLocked();
        encoderNodes = collectEncoderNodesLocked();
    }

    if (wasRunning) {
        /* 先 join C capture 线程，保证之后没有新 PCM publish 到 Node。 */
        audio_capture_stop(m_capture);
    }
    for (const std::shared_ptr<EncoderNode>& node : encoderNodes) {
        node->stop();
    }
    for (const std::shared_ptr<ApmNode>& node : apmNodes) {
        node->stop();
    }
    if (m_capture != nullptr) {
        audio_capture_close(m_capture);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_captureFormat = {};
        m_capturePeriodFrames = 0;
        m_rawFramePool.reset();
    }
}

bool AudioPipeline::isCaptureRunning() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_captureRunning;
}

AudioPcmFormat AudioPipeline::captureFormat() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_captureFormat;
}

AudioSubscription AudioPipeline::subscribePcm(const AudioPcmRequest& request, PcmCallback callback)
{
    if (!callback) {
        return {};
    }

    std::shared_ptr<AudioPcmHub> hub;
    std::shared_ptr<KeepAlive> keepAlive;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!request.useApm) {
            hub = m_rawPcmHub;
        } else {
            const std::shared_ptr<ApmNode> node = findOrCreateApmNodeLocked(request);
            if (!node) {
                return {};
            }
            hub = node->outputHub();
            keepAlive = std::make_shared<KeepAlive>();
            keepAlive->nodes.push_back(node);
        }
    }

    auto holder = std::make_shared<AudioPcmHub::Subscription>(hub->subscribe(std::move(callback)));
    if (!holder->valid()) {
        return {};
    }
    return AudioSubscription([holder] { holder->reset(); }, keepAlive);
}

AudioSubscription AudioPipeline::subscribeEncoded(const AudioEncodedRequest& request,
                                                   EncodedPacketCallback callback)
{
    if (!callback) {
        return {};
    }

    std::shared_ptr<AudioEncodedPacketHub> hub;
    auto keepAlive = std::make_shared<KeepAlive>();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::shared_ptr<AudioPcmHub> inputHub = m_rawPcmHub;
        if (request.useApm) {
            AudioPcmRequest apmRequest;
            apmRequest.useApm = true;
            apmRequest.apmConfig = request.apmConfig;
            apmRequest.apmWarmupDiscardDurationMs = request.apmWarmupDiscardDurationMs;
            const std::shared_ptr<ApmNode> apmNode = findOrCreateApmNodeLocked(apmRequest);
            if (!apmNode) {
                return {};
            }
            inputHub = apmNode->outputHub();
            keepAlive->nodes.push_back(apmNode);
        }

        const std::shared_ptr<EncoderNode> encoderNode = findOrCreateEncoderNodeLocked(inputHub, request);
        if (!encoderNode) {
            return {};
        }
        keepAlive->nodes.push_back(encoderNode);
        hub = encoderNode->outputHub();
    }

    auto holder = std::make_shared<AudioEncodedPacketHub::Subscription>(hub->subscribe(std::move(callback)));
    if (!holder->valid()) {
        return {};
    }
    return AudioSubscription([holder] { holder->reset(); }, keepAlive);
}

std::string AudioPipeline::lastError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

void AudioPipeline::onCapturedPcm(const AudioPcmFrame* frame, void* userData)
{
    auto* pipeline = static_cast<AudioPipeline*>(userData);
    if (pipeline != nullptr && frame != nullptr) {
        pipeline->publishCapturedPcm(*frame);
    }
}

void AudioPipeline::publishCapturedPcm(const AudioPcmFrame& frame)
{
    /* 没有 raw 订阅者、APM Node 或 Encoder Node 时，复制这块 PCM 没有意义。 */
    if (m_rawPcmHub->subscriberCount() == 0) {
        return;
    }
    const AudioFramePtr copied = m_rawFramePool ? m_rawFramePool->copyFrom(frame) : nullptr;
    if (!copied) {
        ++m_droppedRawFramesByPool;
        if (m_droppedRawFramesByPool == 1 || m_droppedRawFramesByPool % 100 == 0) {
            LOG_WARN("AudioPipeline", "raw PCM 池已满，累计丢弃=" << m_droppedRawFramesByPool
                                                                       << " poolCapacity="
                                                                       << (m_rawFramePool
                                                                               ? m_rawFramePool->capacity()
                                                                               : 0));
        }
        return;
    }
    m_rawPcmHub->publish(copied);
}

std::shared_ptr<AudioPipeline::ApmNode> AudioPipeline::findOrCreateApmNodeLocked(const AudioPcmRequest& request)
{
    for (auto it = m_apmNodes.begin(); it != m_apmNodes.end();) {
        if (const std::shared_ptr<ApmNode> node = it->lock()) {
            if (node->matches(request)) {
                return node;
            }
            ++it;
        } else {
            it = m_apmNodes.erase(it);
        }
    }

    auto node = std::make_shared<ApmNode>(request.apmConfig,
                                          request.apmWarmupDiscardDurationMs,
                                          m_config.defaultNodeQueueCapacity,
                                          m_config.pcmFramePoolCapacity);
    /* 先建立输入连接，再启动 worker；启动前到达的 PCM 明确按 Node 未运行处理，不积压。 */
    node->attachInput(m_rawPcmHub);
    if (m_captureRunning && !startNodeLocked(node)) {
        return nullptr;
    }
    m_apmNodes.push_back(node);
    return node;
}

std::shared_ptr<AudioPipeline::EncoderNode> AudioPipeline::findOrCreateEncoderNodeLocked(
    const std::shared_ptr<AudioPcmHub>& inputHub,
    const AudioEncodedRequest& request)
{
    AudioEncodedRequest normalizedRequest = request;
    if (normalizedRequest.inputQueueCapacity == 0) {
        normalizedRequest.inputQueueCapacity = m_config.defaultNodeQueueCapacity;
    }

    for (auto it = m_encoderNodes.begin(); it != m_encoderNodes.end();) {
        if (const std::shared_ptr<EncoderNode> node = it->lock()) {
            if (node->matches(inputHub, normalizedRequest)) {
                return node;
            }
            ++it;
        } else {
            it = m_encoderNodes.erase(it);
        }
    }

    auto node = std::make_shared<EncoderNode>(inputHub,
                                              normalizedRequest.encoderConfig,
                                              normalizedRequest.inputQueueCapacity,
                                              m_config.encodedPacketPoolCapacity);
    /* 同样先建立输入连接，再启动编码 worker；启动前到达的 PCM 不积压。 */
    node->attachInput();
    if (m_captureRunning && !startNodeLocked(node)) {
        return nullptr;
    }
    m_encoderNodes.push_back(node);
    return node;
}

std::vector<std::shared_ptr<AudioPipeline::ApmNode>> AudioPipeline::collectApmNodesLocked()
{
    std::vector<std::shared_ptr<ApmNode>> nodes;
    for (auto it = m_apmNodes.begin(); it != m_apmNodes.end();) {
        if (const std::shared_ptr<ApmNode> node = it->lock()) {
            nodes.push_back(node);
            ++it;
        } else {
            it = m_apmNodes.erase(it);
        }
    }
    return nodes;
}

std::vector<std::shared_ptr<AudioPipeline::EncoderNode>> AudioPipeline::collectEncoderNodesLocked()
{
    std::vector<std::shared_ptr<EncoderNode>> nodes;
    for (auto it = m_encoderNodes.begin(); it != m_encoderNodes.end();) {
        if (const std::shared_ptr<EncoderNode> node = it->lock()) {
            nodes.push_back(node);
            ++it;
        } else {
            it = m_encoderNodes.erase(it);
        }
    }
    return nodes;
}

bool AudioPipeline::startNodeLocked(const std::shared_ptr<ApmNode>& node)
{
    if (node->start(m_captureFormat, m_capturePeriodFrames)) {
        return true;
    }
    setErrorLocked("启动 APM Node 失败: " + node->lastError());
    return false;
}

bool AudioPipeline::startNodeLocked(const std::shared_ptr<EncoderNode>& node)
{
    if (node->start(m_captureFormat, m_capturePeriodFrames)) {
        return true;
    }
    setErrorLocked("启动 Encoder Node 失败: " + node->lastError());
    return false;
}

void AudioPipeline::setErrorLocked(const std::string& message)
{
    m_lastError = message;
    LOG_ERROR("AudioPipeline", message);
}
