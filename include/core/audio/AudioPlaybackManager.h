#ifndef MCR_AUDIO_PLAYBACK_MANAGER_H
#define MCR_AUDIO_PLAYBACK_MANAGER_H

/*
 * 下行音频总控：网络/文件交付编码包 -> 短队列 -> 单播放线程 -> 解码 -> ALSA。
 *
 * AudioPlaybackManager 不处理 RTP 乱序、jitter buffer 或混音。这些是协议/通话层的
 * 职责；调用 enqueue_packet() 前，调用方必须已经按播放顺序交付编码包。
 *
 * 如果 captureManager 非空，播放线程会在每个 10ms PCM 块真正写 ALSA 前复制一份
 * 给 AudioCaptureManager。AEC3 关闭时 capture manager 将其作为无操作处理，因此
 * PlaybackManager 不需要理解 AEC 的启停状态。
 */

#include "AudioCaptureManager.h"
#include "AudioDecoder.h"
#include "AudioPlayback.h"

#include <pthread.h>
#include <stdint.h>

enum {
    /* 默认缓存 8 个 20ms Opus 包，约 160ms；这是播放队列，不是网络 jitter buffer。 */
    AUDIO_PLAYBACK_MANAGER_DEFAULT_MAX_QUEUED_PACKETS = 8,
    /* 初次启动先积累 3 个 20ms 包（约 60ms）再写 ALSA，避免从 10ms 空硬件队列起播。 */
    AUDIO_PLAYBACK_MANAGER_STARTUP_QUEUED_PACKETS = 3,
    /* RTP/Opus 常规 MTU 包远小于此值；保留余量同时避免网络输入无限制占内存。 */
    AUDIO_PLAYBACK_MANAGER_DEFAULT_MAX_PACKET_BYTES = 4096,
    /* Opus 单次解码最大 120ms；加一个 10ms block 容纳跨包残余。 */
    AUDIO_PLAYBACK_MANAGER_MAX_PENDING_PCM_FRAMES = 6240,
};

typedef struct AudioPlaybackManagerConfig {
    /* 当前 AudioDecoder 已实现 Opus；G.711/AAC 后续只需扩展 AudioDecoder。 */
    AudioCodec codec;

    /* 解码后的项目内 PCM 格式。第一版默认 48kHz / 单声道 / S16_LE。 */
    AudioPcmFormat outputFormat;

    /* ALSA 期望 period。默认 480 frames，即 48kHz 下 10ms，与 APM 对齐。 */
    snd_pcm_uframes_t requestedPeriodFrames;

    /* 固定容量编码包队列。满时 manager 丢最旧包以控制语音延迟。 */
    size_t maxQueuedPackets;
    size_t maxPacketBytes;

    /* 可为空。非空时，播放的 10ms PCM 会同时投递为采集侧 AEC reference。 */
    AudioCaptureManager *captureManager;
} AudioPlaybackManagerConfig;

typedef struct AudioPlaybackManager {
    AudioPlayback playback;
    AudioDecoder decoder;
    AudioPlaybackManagerConfig config;

    /* 一个线程独占 decoder 和 ALSA；网络线程只能向下方队列复制包。 */
    pthread_t playbackThread;
    pthread_mutex_t queueMutex;
    pthread_cond_t queueCondition;
    int queueSynchronizationInitialized;
    int threadCreated;
    int stopRequested;
    int playbackStarted;

    /*
     * 固定容量环形编码包队列。每个槽位占 maxPacketBytes，避免实时路径频繁 malloc。
     * playbackStarted 前仅用于声卡初次预填充；它不排序、也不理解 RTP，因此不属于
     * jitter buffer。网络乱序、迟到和 PLC 仍由未来通话/协议层负责。
     */
    uint8_t *packetStorage;
    size_t *packetSizes;
    uint64_t *packetTimestampsUs;
    uint32_t *packetDurationsUs;
    uint8_t *dequeueBuffer;
    size_t queueHead;
    size_t queueCount;

    /* 解码后的 PCM 可能不是恰好 10ms；这里把残余拼成 APM/reference 所需的 10ms block。 */
    uint8_t *pendingPcmStorage;
    size_t pendingPcmFrames;
    uint64_t pendingPcmTimestampUs;
    uint64_t pendingPcmTimestampRemainder;

    /* 运行统计：仅用于日志/健康检查，不参与媒体逻辑。 */
    uint64_t droppedPacketCount;
    uint64_t decodedPacketCount;
    uint64_t playedPcmFrames;
    uint64_t playbackWriteFailures;
    int initialized;
} AudioPlaybackManager;

void audio_playback_manager_config_init(AudioPlaybackManagerConfig *config);
int audio_playback_manager_init(AudioPlaybackManager *manager,
                                const AudioPlaybackManagerConfig *config);
int audio_playback_manager_start(AudioPlaybackManager *manager);

/*
 * 网络/协议线程的唯一入口。函数只复制编码包进入有界队列，不做解码、APM 或 ALSA 写入。
 * 当队列已满时淘汰最旧包并返回 0，以低延迟优先；非法包或未启动时才返回负 errno。
 */
int audio_playback_manager_enqueue_packet(AudioPlaybackManager *manager,
                                          const AudioEncodedPacket *packet);

/* 停止时立即丢弃尚未播放的旧音频；不会为了 drain 队列而延长通话退出时间。 */
void audio_playback_manager_stop(AudioPlaybackManager *manager);
void audio_playback_manager_close(AudioPlaybackManager *manager);

#endif
