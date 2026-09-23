#include "AudioPlaybackPipeline.hpp"

#include "Log.hpp"

#include <algorithm>
#include <cerrno>
#include <utility>

namespace {

uint64_t pcmDurationUs(const AudioFrame& frame)
{
    if (frame.format.sampleRate == 0) {
        return 0;
    }
    return static_cast<uint64_t>(frame.frames) * 1000000ULL / frame.format.sampleRate;
}

uint64_t encodedDurationUs(const EncodedAudioPacket& packet)
{
    /* 优先由准确 sample 数换算；AAC-LC 的 1024 samples 不能被错误当成固定 21ms。 */
    if (packet.frameSamples != 0 && packet.sourceFormat.sampleRate != 0) {
        return static_cast<uint64_t>(packet.frameSamples) * 1000000ULL
            / packet.sourceFormat.sampleRate;
    }
    /* 旧来源未填写 frameSamples 时，仍兼容它的 duration；最后才按 Opus 常见 20ms 兜底。 */
    return packet.durationUs == 0 ? 20000ULL : packet.durationUs;
}

bool samePcmFormat(const AudioPcmFormat& left, const AudioPcmFormat& right)
{
    return left.sampleRate == right.sampleRate && left.channels == right.channels
        && left.sampleFormat == right.sampleFormat;
}

} // namespace

AudioPlaybackPipelineConfig::AudioPlaybackPipelineConfig()
{
    audio_playback_config_init(&playback);
    /* 项目内 APM/reference 的最小时间单位是 10ms，播放侧也保持这个 period。 */
    playback.requestedPeriodFrames = playback.requestedFormat.sampleRate / 100;
    /* 硬件总缓冲设为 8 个 period（80ms），与底层 start_threshold（70ms 起播门限）配合防 XRUN。 */
    playback.requestedBufferFrames = playback.requestedPeriodFrames * 8;
}

AudioPlaybackPipeline::~AudioPlaybackPipeline()
{
    stop();
}

bool AudioPlaybackPipeline::start(const AudioPlaybackPipelineConfig& config)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running) {
        return true;
    }
    if (config.maxQueuedItems == 0 || config.maxQueuedDurationUs == 0
        || config.playback.requestedFormat.sampleRate == 0
        || config.playback.requestedFormat.channels == 0
        || config.playback.requestedPeriodFrames == 0) {
        m_lastError = "播放配置无效";
        return false;
    }

    m_config = config;
    const int openResult = audio_playback_open_auto(&m_playback, &m_config.playback);
    if (openResult < 0) {
        m_lastError = "打开 ALSA 播放设备失败: " + std::to_string(openResult);
        return false;
    }

    m_queue.assign(m_config.maxQueuedItems, {});
    clearQueueLocked();
    m_inputMode = InputMode::None;
    m_stopRequested = false;
    m_acceptedItems = 0;
    m_droppedQueuedItems = 0;
    m_decodedPackets = 0;
    m_playedPcmFrames = 0;
    m_decodeFailures = 0;
    m_playbackFailures = 0;
    audio_decoder_set_pcm_callback(&m_decoder, &AudioPlaybackPipeline::onDecodedPcm, this);
    m_running = true;
    m_lastError.clear();
    m_worker = std::thread(&AudioPlaybackPipeline::workerMain, this);
    LOG_INFO("AudioPlaybackPipeline", "扬声器播放已启动 PCM=" << m_playback.actualFormat.sampleRate
                                                                     << "Hz/"
                                                                     << m_playback.actualFormat.channels
                                                                     << "ch queue=" << m_config.maxQueuedDurationUs / 1000
                                                                     << "ms prebuffer="
                                                                     << m_config.startupPrebufferDurationUs / 1000 << "ms");
    return true;
}

void AudioPlaybackPipeline::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) {
            return;
        }
        m_stopRequested = true;
        clearQueueLocked();
        m_cv.notify_all();
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    audio_decoder_close(&m_decoder);
    audio_playback_close(&m_playback);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
        m_stopRequested = false;
        m_inputMode = InputMode::None;
        m_queue.clear();
    }
}

bool AudioPlaybackPipeline::isRunning() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_running;
}

bool AudioPlaybackPipeline::push(AudioFramePtr pcm)
{
    return enqueuePcm(std::move(pcm));
}

bool AudioPlaybackPipeline::push(EncodedAudioPacketPtr packet)
{
    return enqueueEncoded(std::move(packet));
}

AudioPcmFormat AudioPlaybackPipeline::outputFormat() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_playback.actualFormat;
}

AudioPlaybackPipelineStatistics AudioPlaybackPipeline::statistics() const
{
    AudioPlaybackPipelineStatistics statistics {};
    statistics.acceptedItems = m_acceptedItems.load();
    statistics.droppedQueuedItems = m_droppedQueuedItems.load();
    statistics.decodedPackets = m_decodedPackets.load();
    statistics.playedPcmFrames = m_playedPcmFrames.load();
    statistics.decodeFailures = m_decodeFailures.load();
    statistics.playbackFailures = m_playbackFailures.load();
    return statistics;
}

std::string AudioPlaybackPipeline::lastError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

bool AudioPlaybackPipeline::enqueuePcm(AudioFramePtr pcm)
{
    if (!pcm || pcm->frames == 0 || pcm->format.sampleRate == 0) {
        return false;
    }
    QueueItem item;
    item.kind = ItemKind::Pcm;
    item.durationUs = pcmDurationUs(*pcm);
    item.pcm = std::move(pcm);
    std::lock_guard<std::mutex> lock(m_mutex);
    return enqueueItemLocked(std::move(item), InputMode::Pcm);
}

bool AudioPlaybackPipeline::enqueueEncoded(EncodedAudioPacketPtr packet)
{
    if (!packet || packet->codec == AUDIO_CODEC_UNKNOWN || packet->bytes.empty()) {
        return false;
    }
    QueueItem item;
    item.kind = ItemKind::Encoded;
    item.durationUs = encodedDurationUs(*packet);
    item.encoded = std::move(packet);
    std::lock_guard<std::mutex> lock(m_mutex);
    return enqueueItemLocked(std::move(item), InputMode::Encoded);
}

bool AudioPlaybackPipeline::enqueueItemLocked(QueueItem item, InputMode inputMode)
{
    if (!m_running || m_stopRequested) {
        return false;
    }
    if (m_inputMode == InputMode::None) {
        m_inputMode = inputMode;
    } else if (m_inputMode != inputMode) {
        m_lastError = "同一播放实例不能混合 PCM 与压缩音频；请 stop() 后切换输入模式";
        LOG_WARN("AudioPlaybackPipeline", m_lastError);
        return false;
    }

    while (m_queueCount != 0
           && (m_queueCount == m_queue.size()
               || m_queuedDurationUs + item.durationUs > m_config.maxQueuedDurationUs)) {
        QueueItem& oldest = m_queue[m_queueHead];
        m_queuedDurationUs -= std::min(m_queuedDurationUs, oldest.durationUs);
        oldest = {};
        m_queueHead = (m_queueHead + 1) % m_queue.size();
        --m_queueCount;
        ++m_droppedQueuedItems;
    }

    const size_t tail = (m_queueHead + m_queueCount) % m_queue.size();
    m_queue[tail] = std::move(item);
    m_queuedDurationUs += m_queue[tail].durationUs;
    ++m_queueCount;
    ++m_acceptedItems;
    m_cv.notify_one();
    return true;
}

void AudioPlaybackPipeline::clearQueueLocked()
{
    for (QueueItem& item : m_queue) {
        item = {};
    }
    m_queueHead = 0;
    m_queueCount = 0;
    m_queuedDurationUs = 0;
    m_playbackStarted = false;
}

void AudioPlaybackPipeline::workerMain()
{
    while (true) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lock(m_mutex);

			//首次播放时先在队列中存 m_config.startupPrebufferDurationUs 的音频包，
            m_cv.wait(lock, [this] {
                return m_stopRequested || (m_queueCount != 0
                                           && (m_playbackStarted
                                               || m_queuedDurationUs >= m_config.startupPrebufferDurationUs));
            });
            if (m_stopRequested) {
                return;
            }
            m_playbackStarted = true;//标记开始播放
            item = std::move(m_queue[m_queueHead]);
            m_queuedDurationUs -= std::min(m_queuedDurationUs, item.durationUs);
            m_queue[m_queueHead] = {};
            m_queueHead = (m_queueHead + 1) % m_queue.size();
            --m_queueCount;
        }

		//如果是pcm直接播放
        if (item.kind == ItemKind::Pcm) {
            if (!item.pcm || !playPcm(item.pcm->pcmView())) {
                ++m_playbackFailures;
            }
            continue;
        }

		//说明是压缩格式需要解码在播放
        AudioEncodedPacket packet {};
        if (item.encoded) {
            packet = item.encoded->packetView();
        }
        if (!item.encoded || !ensureDecoder(*item.encoded)
            || audio_decoder_decode_packet(&m_decoder, &packet) < 0) {
            ++m_decodeFailures;
            if (item.encoded) {
                LOG_WARN("AudioPlaybackPipeline", "解码压缩音频失败 codec=" << item.encoded->codec
                                                                                   << " bytes="
                                                                                   << item.encoded->bytes.size());
            }
            continue;
        }
        ++m_decodedPackets;
    }
}

bool AudioPlaybackPipeline::ensureDecoder(const EncodedAudioPacket& packet)
{
    if (packet.sourceFormat.sampleRate == 0 || packet.sourceFormat.channels == 0
        || packet.sourceFormat.sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE) {
        setError("压缩音频包缺少有效 sourceFormat");
        return false;
    }
    if (m_decoder.codec == packet.codec && m_decoder.implementation != nullptr
        && samePcmFormat(m_decoderOutputFormat, packet.sourceFormat)) {
        return true;
    }
    audio_decoder_close(&m_decoder);
    m_decoderOutputFormat = {};
    audio_decoder_set_pcm_callback(&m_decoder, &AudioPlaybackPipeline::onDecodedPcm, this);
    const int result = audio_decoder_init(&m_decoder, packet.codec, &packet.sourceFormat);
    if (result < 0) {
        setError("初始化音频解码器失败 codec=" + std::to_string(packet.codec)
                 + " error=" + std::to_string(result));
        return false;
    }
    m_decoderOutputFormat = packet.sourceFormat;
    LOG_INFO("AudioPlaybackPipeline", "音频解码器已切换 codec=" << packet.codec
                                                                    << " PCM="
                                                                    << packet.sourceFormat.sampleRate
                                                                    << "Hz/"
                                                                    << packet.sourceFormat.channels << "ch");
    return true;
}

bool AudioPlaybackPipeline::playPcm(const AudioPcmFrame& frame)
{
    const int result = audio_playback_write_pcm(&m_playback, &frame);
    if (result < 0) {
        setError("写入 ALSA 失败: " + std::to_string(result));
        return false;
    }
    m_playedPcmFrames += frame.frames;
    return true;
}

int AudioPlaybackPipeline::onDecodedPcm(const AudioPcmFrame* frame, void* userData)
{
    auto* pipeline = static_cast<AudioPlaybackPipeline*>(userData);
    if (pipeline == nullptr || frame == nullptr) {
        return -EINVAL;
    }
    return pipeline->playPcm(*frame) ? 0 : -EIO;
}

void AudioPlaybackPipeline::setError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastError = message;
    LOG_ERROR("AudioPlaybackPipeline", message);
}
