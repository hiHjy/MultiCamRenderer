#include "AudioPipeline.hpp"

#include "AudioAac.h"
#include "Log.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
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
        && left.frameSamples == right.frameSamples
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

bool makeEncoderConfig(AudioCodec codec, AudioBitratePreset preset, AudioEncoderConfig& config)
{
    audio_encoder_config_init(&config);
    config.codec = codec;

    switch (codec) {
    case AUDIO_CODEC_AAC:
        /* AAC-LC 原始 access unit 固定覆盖 1024 samples；64/96/128k 是常用监控档位。 */
        config.bitrate = preset == AudioBitratePreset::Low ? 64000
            : preset == AudioBitratePreset::Medium ? 96000
                                                  : 128000;
        config.frameDurationUs = 0;
        config.frameSamples = AUDIO_AAC_LC_FRAME_SAMPLES;
        config.maxPacketBytes = 2048;
        return true;

    case AUDIO_CODEC_OPUS:
        /* Opus VOIP 使用 20ms 包；16/32/64k 分别覆盖低带宽、普通语音和高质量语音。 */
        config.bitrate = preset == AudioBitratePreset::Low ? 16000
            : preset == AudioBitratePreset::Medium ? 32000
                                                  : 64000;
        config.frameDurationUs = 20000;
        config.frameSamples = 0;
        config.maxPacketBytes = 1200;
        return true;

    case AUDIO_CODEC_UNKNOWN:
    case AUDIO_CODEC_G711A:
    case AUDIO_CODEC_G711U:
        return false;
    }
    return false;
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
        m_droppedInputFrames = 0;
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
        uint64_t droppedCount = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_stopRequested) {
                return;
            }
            if (m_queue.size() == m_queueCapacity) {
                /* APM 跟不上采集时追实时：淘汰最旧 10ms PCM，而不是继续累计延迟。 */
                m_queue.pop_front();
                droppedCount = ++m_droppedInputFrames;
            }
            m_queue.push_back(std::move(frame));
            m_cv.notify_one();
        }
        if (droppedCount != 0 && (droppedCount == 1 || droppedCount % 100 == 0)) {
            LOG_WARN("ApmNode", "APM 输入 PCM 队列已满，淘汰旧 10ms 帧累计=" << droppedCount
                                << " queueCapacity=" << m_queueCapacity);
        }
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
        m_droppedInputFrames = 0;
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
        uint64_t droppedCount = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running || m_stopRequested) {
                return;
            }
            if (m_queue.size() == m_queueCapacity) {
                /* 编码跟不上采集时追实时；worker 会据时间戳断裂重建 AAC/Opus 状态。 */
                m_queue.pop_front();
                droppedCount = ++m_droppedInputFrames;
            }
            m_queue.push_back(std::move(frame));
            m_cv.notify_one();
        }
        if (droppedCount != 0 && (droppedCount == 1 || droppedCount % 100 == 0)) {
            LOG_WARN("EncoderNode", "编码输入 PCM 队列已满，淘汰旧 10ms 帧累计=" << droppedCount
                                    << " queueCapacity=" << m_queueCapacity);
        }
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

	//默认 480 x 2 x 1 = 960B
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

/*
 * =========================================================================================
 * 订阅线路 1：subscribePcm (订阅 PCM 原始/降噪后的音频帧)
 *
 * 【业务调用示例】：
 *   AudioPcmRequest req;
 *   req.useApm = true; // 是否开启 3A (AEC/ANS/AGC)
 *   AudioSubscription sub = pipeline.subscribePcm(req, [](AudioFramePtr frame) { ... });
 *
 * 【核心设计】：
 *   1. 若无需 APM，直接挂在采集源的 m_rawPcmHub 上；
 *   2. 若需 APM，复用或新建一个 ApmNode，挂在 ApmNode->outputHub() 上；
 *   3. 返回 AudioSubscription 句柄，内部通过 keepAlive 强引用 ApmNode，实现 RAII 生命周期绑定。
 * =========================================================================================
 */
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
            // 分支 A：不需要 3A 处理，直接订阅麦克风原始采集的 rawPcmHub
            hub = m_rawPcmHub;
        } else {
            // 分支 B：需要 3A 处理，复用或创建对应的 ApmNode
            const std::shared_ptr<ApmNode> node = findOrCreateApmNodeLocked(request);
            if (!node) {
                return {};
            }
            // 订阅的数据来源切换为该 ApmNode 处理后的 outputHub
            hub = node->outputHub();

            // 将 ApmNode 的 shared_ptr 放入 keepAlive 结构体中
            // 只要最终返回的 AudioSubscription 活着，这个 ApmNode 就不会被析构销毁
            keepAlive = std::make_shared<KeepAlive>();
            keepAlive->nodes.push_back(node);
        }
    }

    // 在目标 Hub 上注册回调函数；返回底层 Hub 的 Subscription 句柄
	//hub->subscribe(std::move(callback) 返回 AudioPcmHub::Subscription对象
    std::shared_ptr<AudioPcmHub::Subscription> holder = std::make_shared<AudioPcmHub::Subscription>(hub->subscribe(std::move(callback)));
    if (!holder->valid()) {
        return {};
    }

    // 封装并返回高阶句柄 AudioSubscription：
    // - 第一个参数是退订动作：当句柄析构时调用 holder->reset() 取消 Hub 回调；
    // - 第二个参数是保活指针 keepAlive：句柄析构时释放 Node 强引用，若无其他人使用该 Node，Node 自动销毁退出。
    return AudioSubscription([holder] { holder->reset(); }, keepAlive);
}

/*
 * =========================================================================================
 * 订阅线路 2：subscribeEncoded (订阅 Opus 编码压缩包，用于 RTSP 推流或网络传输)
 *
 * 【数据流水线拓扑】：
 *   声卡采集 (rawPcmHub) -> [ApmNode 3A处理(可选)] -> EncoderNode (Opus编码) -> 业务回调
 *
 * 【核心设计】：
 *   1. 确定输入源：rawPcmHub 或 ApmNode->outputHub()；
 *   2. 挂载 EncoderNode：复用或新建对应编码参数的 EncoderNode，输入端接在上面的源头上；
 *   3. 将业务回调挂在 EncoderNode->outputHub()；
 *   4. 返回 AudioSubscription 句柄，同时把 ApmNode 和 EncoderNode 塞入 keepAlive 保活。
 * =========================================================================================
 */
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

        // 步骤 1：默认输入源为原始麦克风 PCM
        std::shared_ptr<AudioPcmHub> inputHub = m_rawPcmHub;

        // 步骤 2：若请求开启 APM，先串入 ApmNode
        if (request.useApm) {
            AudioPcmRequest apmRequest;
            apmRequest.useApm = true;
            apmRequest.apmConfig = request.apmConfig;
            apmRequest.apmWarmupDiscardDurationMs = request.apmWarmupDiscardDurationMs;
            const std::shared_ptr<ApmNode> apmNode = findOrCreateApmNodeLocked(apmRequest);
            if (!apmNode) {
                return {};
            }
            // 编码器的输入源切换为 ApmNode 的输出
            inputHub = apmNode->outputHub();
            // ApmNode 纳入保活名单
            keepAlive->nodes.push_back(apmNode);
        }

        // 步骤 3：在输入源后面拼接 EncoderNode (Opus 编码器节点)
        const std::shared_ptr<EncoderNode> encoderNode = findOrCreateEncoderNodeLocked(inputHub, request);
        if (!encoderNode) {
            return {};
        }
        // EncoderNode 纳入保活名单
        keepAlive->nodes.push_back(encoderNode);

        // 最终业务回调要监听的是 EncoderNode 压出 Opus 包后的 outputHub
        hub = encoderNode->outputHub();
    }

    // 在编码包 Hub 上注册业务回调函数
    auto holder = std::make_shared<AudioEncodedPacketHub::Subscription>(hub->subscribe(std::move(callback)));
    if (!holder->valid()) {
        return {};
    }

    // 返回 RAII 句柄：
    // - 只要外部持有该返回值，整条链路（ApmNode + EncoderNode）就保持运行；
    // - 外部一旦丢弃或重置该返回值，holder 退订、keepAlive 释放，链条上的节点自动停止并回收。
    return AudioSubscription([holder] { holder->reset(); }, keepAlive);
}

AudioSubscription AudioPipeline::subscribeEncoded(AudioCodec codec,
                                                   AudioBitratePreset preset,
                                                   EncodedPacketCallback callback)
{
    AudioEncodedRequest request;
    if (!makeEncoderConfig(codec, preset, request.encoderConfig)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setErrorLocked("不支持按预设创建的音频编码器");
        return {};
    }
    return subscribeEncoded(request, std::move(callback));
}

bool AudioPipeline::getEncodedStreamInfo(AudioCodec codec,
                                         AudioBitratePreset preset,
                                         AudioEncodedStreamInfo& info) const
{
    AudioEncoderConfig config {};
    if (!makeEncoderConfig(codec, preset, config)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setErrorLocked("不支持按预设创建的音频编码器");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_captureRunning || m_captureFormat.sampleRate == 0 || m_captureFormat.channels == 0
        || m_captureFormat.sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE) {
        setErrorLocked("音频采集尚未启动，无法取得实际 PCM 格式");
        return false;
    }

    info.codec = codec;
    info.sourceFormat = m_captureFormat;
    info.bitrate = config.bitrate;
    info.frameSamples = config.frameSamples;
    return true;
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

/*
 * -----------------------------------------------------------------------------------------
 * 辅助函数：findOrCreateApmNodeLocked (查找或创建 ApmNode)
 *
 * 【为什么内部存的是 weak_ptr？】
 *   AudioPipeline 自己不强行霸占 ApmNode 的生命周期，而是使用弱引用 weak_ptr。
 *   - 只要外部有人在订阅（持有 AudioSubscription -> keepAlive 强引用），这个 ApmNode 就活着；
 *   - 一旦所有订阅者都退出了，ApmNode 自动析构；这里遍历时 lock() 就会变空，自动从列表中剔除。
 * -----------------------------------------------------------------------------------------
 */
std::shared_ptr<AudioPipeline::ApmNode> AudioPipeline::findOrCreateApmNodeLocked(const AudioPcmRequest& request)
{
    // 步骤 1：遍历已有缓存的 ApmNode 弱引用列表
    for (auto it = m_apmNodes.begin(); it != m_apmNodes.end();) {
        if (const std::shared_ptr<ApmNode> node = it->lock()) {
            // 该节点依然存活，检查其 3A 配置与预热丢弃参数是否和请求完全一致
            if (node->matches(request)) {
                // 完美命中！直接复用该节点，避免重复创建多个相同的 WebRTC 实例消耗 CPU
                return node;
            }
            ++it;
        } else {
            // 该弱引用指向的节点已经死掉了（所有订阅者都已退订），顺手把它从列表中清理掉
            it = m_apmNodes.erase(it);
        }
    }

    // 步骤 2：没有找到可复用的节点，新实例化一个 ApmNode
    auto node = std::make_shared<ApmNode>(request.apmConfig,
                                          request.apmWarmupDiscardDurationMs,
                                          m_config.defaultNodeQueueCapacity,
                                          m_config.pcmFramePoolCapacity);
    /* 先建立输入连接，再启动 worker；启动前到达的 PCM 明确按 Node 未运行处理，不积压。 */
    node->attachInput(m_rawPcmHub);
    if (m_captureRunning && !startNodeLocked(node)) {
        return nullptr;
    }
    // 存入 weak_ptr 列表方便后续其他订阅者复用
    m_apmNodes.push_back(node);
    return node;
}

/*
 * -----------------------------------------------------------------------------------------
 * 辅助函数：findOrCreateEncoderNodeLocked (查找或创建 EncoderNode)
 *
 * 逻辑同上：按 (输入源 inputHub + 编码参数) 查找是否已有运行中的编码器节点；
 * 有则复用，无则新建并挂在对应 inputHub 下。
 * -----------------------------------------------------------------------------------------
 */
std::shared_ptr<AudioPipeline::EncoderNode> AudioPipeline::findOrCreateEncoderNodeLocked(
    const std::shared_ptr<AudioPcmHub>& inputHub,
    const AudioEncodedRequest& request)
{
    AudioEncodedRequest normalizedRequest = request;
    if (normalizedRequest.inputQueueCapacity == 0) {
        normalizedRequest.inputQueueCapacity = m_config.defaultNodeQueueCapacity;
    }

    // 步骤 1：遍历查找可复用的 EncoderNode
    for (auto it = m_encoderNodes.begin(); it != m_encoderNodes.end();) {
        if (const std::shared_ptr<EncoderNode> node = it->lock()) {
            // 检查输入源 Hub 是否相同，且编码参数是否匹配
            if (node->matches(inputHub, normalizedRequest)) {
                return node;
            }
            ++it;
        } else {
            // 清理已死亡的弱引用
            it = m_encoderNodes.erase(it);
        }
    }

    // 步骤 2：未找到则创建新的 Opus 编码器节点
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

void AudioPipeline::setErrorLocked(const std::string& message) const
{
    m_lastError = message;
    LOG_ERROR("AudioPipeline", message);
}
