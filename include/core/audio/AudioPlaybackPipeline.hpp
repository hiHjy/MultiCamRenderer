#pragma once

#include "AudioDecoder.h"
#include "AudioFrame.hpp"
#include "AudioPlayback.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* 播放管线启动参数：一个实例对应项目内唯一的扬声器输出拥有者。 */
struct AudioPlaybackPipelineConfig {
    /* AudioPlayback 仍负责 default 设备选择、声道转换与 ALSA XRUN 恢复。 */
    AudioPlaybackConfig playback {};

    /* 上游最多可积压的音频时长；超过时丢最旧项，优先保持实时。 */
    uint64_t maxQueuedDurationUs = 100000;
    /* 队列的硬项目数上限，防御 duration=0 或异常 duration 的输入。 */
    size_t maxQueuedItems = 16;
    /* 初始写声卡前的最小预填充，避免仅一小包 PCM 就触发 ALSA XRUN。 */
    uint64_t startupPrebufferDurationUs = 80000;

    AudioPlaybackPipelineConfig();
};

/* 运行统计只用于健康检查/日志，不参与播放控制。 */
struct AudioPlaybackPipelineStatistics {
    uint64_t acceptedItems = 0;
    uint64_t droppedQueuedItems = 0;
    uint64_t decodedPackets = 0;
    uint64_t playedPcmFrames = 0;
    uint64_t decodeFailures = 0;
    uint64_t playbackFailures = 0;
};

/*
 * 唯一扬声器的 C++ 消费端。
 *
 * push(AudioFramePtr) 接收已经解码的 PCM；push(EncodedAudioPacketPtr) 接收压缩包并由
 * worker 内部按 packet.codec 初始化/切换 AudioDecoder。push() 只执行有界入队；解码和
 * snd_pcm_writei() 永远不在调用方线程执行。
 *
 * 一个已启动的实例只允许一种输入模式（PCM 或 compressed）。若要切换模式，必须先 stop()
 * 再 start()，避免两条无同步时间线争抢同一扬声器；本类不做混音或网络 jitter buffer。
 */
class AudioPlaybackPipeline {
public:
    AudioPlaybackPipeline() = default;
    ~AudioPlaybackPipeline();

    AudioPlaybackPipeline(const AudioPlaybackPipeline&) = delete;
    AudioPlaybackPipeline& operator=(const AudioPlaybackPipeline&) = delete;

    bool start(const AudioPlaybackPipelineConfig& config = AudioPlaybackPipelineConfig());
    void stop();
    bool isRunning() const;

    /* 调用线程只会取得 queue mutex、移动 shared_ptr 并通知 worker。 */
    bool push(AudioFramePtr pcm);
    bool push(EncodedAudioPacketPtr packet);

    AudioPcmFormat outputFormat() const;
    AudioPlaybackPipelineStatistics statistics() const;
    std::string lastError() const;

private:
    enum class InputMode {
        None,
        Pcm,
        Encoded,
    };

    enum class ItemKind {
        Pcm,
        Encoded,
    };

    struct QueueItem {
        ItemKind kind = ItemKind::Pcm;
        AudioFramePtr pcm;
        EncodedAudioPacketPtr encoded;
        uint64_t durationUs = 0;
    };

    bool enqueuePcm(AudioFramePtr pcm);
    bool enqueueEncoded(EncodedAudioPacketPtr packet);
    bool enqueueItemLocked(QueueItem item, InputMode inputMode);
    void clearQueueLocked();
    void workerMain();

    bool ensureDecoder(const EncodedAudioPacket& packet);
    bool playPcm(const AudioPcmFrame& frame);
    static int onDecodedPcm(const AudioPcmFrame* frame, void* userData);
    void setError(const std::string& message);

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<QueueItem> m_queue;
    size_t m_queueHead = 0;
    size_t m_queueCount = 0;
    uint64_t m_queuedDurationUs = 0;
    bool m_playbackStarted = false;
    bool m_stopRequested = false;
    bool m_running = false;
    InputMode m_inputMode = InputMode::None;
    AudioPlaybackPipelineConfig m_config {};
    AudioPlayback m_playback {};
    AudioDecoder m_decoder {};
    AudioPcmFormat m_decoderOutputFormat {};
    std::thread m_worker;
    std::string m_lastError;

    std::atomic<uint64_t> m_acceptedItems { 0 };
    std::atomic<uint64_t> m_droppedQueuedItems { 0 };
    std::atomic<uint64_t> m_decodedPackets { 0 };
    std::atomic<uint64_t> m_playedPcmFrames { 0 };
    std::atomic<uint64_t> m_decodeFailures { 0 };
    std::atomic<uint64_t> m_playbackFailures { 0 };
};
