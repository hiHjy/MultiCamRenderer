#ifndef MCR_AUDIO_CAPTURE_MANAGER_H
#define MCR_AUDIO_CAPTURE_MANAGER_H

#include "AudioCapture.h"
#include "AudioCodec.h"
#include "AudioApm.h"

#include <stdint.h>

/* 32 个 10ms PCM reference = 最多约 320ms。满时淘汰最旧 reference，绝不阻塞播放。 */
enum { AUDIO_CAPTURE_MANAGER_PLAYBACK_REFERENCE_QUEUE_CAPACITY = 32 };

/* 一路上行音频的策略：采集格式、APM 处理与编码方式。 */
typedef struct AudioCaptureManagerConfig {
    AudioCaptureConfig capture;
    AudioApmConfig apm;
    AudioEncoderConfig encoder;

    /*
     * 每次采集启动、或 APM 模式重新创建后，先丢弃的 PCM 时长，单位 ms，默认 500。
     * 这段时间 ALSA 麦克风/模拟前端和 WebRTC 降噪器都可以完成稳定；APM 仍会处理
     * 这些帧，只是不交给编码器，避免首批 RTSP 音频出现突兀的大声或音色跳变。
     */
    uint32_t warmupDiscardDurationMs;
} AudioCaptureManagerConfig;

/*
 * 上行音频管理器：AudioCapture -> AudioApm -> AudioEncoder。
 *
 * 一个实例对应一路麦克风；APM 和编码器都只在内部采集线程调用。
 *
 * 普通监控状态（默认）：
 *   AudioCapture(10ms PCM) -> 原样 PCM -> AudioEncoder -> 上层 packet callback
 *   RTSP 监控优先保留环境声，不默认做语音降噪、高通或自动增益。
 *
 * 双向通话状态：
 *   创建通话后，采集线程在当前 10ms PCM 完成后的安全边界重建 APM：
 *   close -> 以 AEC3 + 高通 + 降噪 open -> 清空旧 playback reference。
 *
 *   远端 Opus -> 解码为 PCM -> 一份送本地扬声器
 *                            -> 一份调用 push_playback_reference() 复制入队
 *   采集线程 -> drain reference queue -> APM reverse(AEC3) -> APM capture -> 编码上行
 *
 * 没有 reference PCM 是正常状态（例如远端安静或未播放），不会中断采集/编码；只是
 * 那段录音没有可消除的扬声器回声。
 */
typedef struct AudioCaptureManager {
    AudioCapture capture;
    AudioApm apm;
    AudioEncoder encoder;
    AudioCaptureManagerConfig config;
    AudioEncodedPacketCallback callback;
    void *callbackUserData;

    /* playback 线程写、采集线程读的固定容量 reference ring queue。数据在入队时复制，
       因此调用方的 AudioPcmFrame 可以在 push 返回后立即释放。 */
    pthread_mutex_t playbackReferenceMutex;
    int playbackReferenceMutexInitialized;
    uint8_t *playbackReferenceStorage;
    uint8_t *playbackReferenceProcessingBuffer;
    size_t playbackReferenceFrameBytes;
    size_t playbackReferenceFrames;
    uint64_t playbackReferenceTimestampsUs[AUDIO_CAPTURE_MANAGER_PLAYBACK_REFERENCE_QUEUE_CAPACITY];
    size_t playbackReferenceHead;
    size_t playbackReferenceCount;
    uint64_t droppedPlaybackReferenceFrames;

    /* 外部线程请求切换 AEC 时写入；真正 close/open APM 的动作只能由采集线程执行。 */
    pthread_mutex_t apmConfigMutex;
    int apmConfigMutexInitialized;
    AudioApmConfig pendingApmConfig;
    int apmReconfigureRequested;

    /* 当前尚需丢弃的处理后 PCM 帧数；只由采集线程读写。 */
    uint64_t warmupFramesRemaining;
    uint64_t warmupDiscardedFrames;
    int started;
    int initialized;
} AudioCaptureManager;

void audio_capture_manager_config_init(AudioCaptureManagerConfig *config);
int audio_capture_manager_init(AudioCaptureManager *manager,
                               const AudioCaptureManagerConfig *config);
void audio_capture_manager_set_packet_callback(AudioCaptureManager *manager,
                                               AudioEncodedPacketCallback callback,
                                               void *userData);
int audio_capture_manager_start(AudioCaptureManager *manager);
void audio_capture_manager_stop(AudioCaptureManager *manager);

/*
 * 请求在采集线程的下一个 10ms 安全边界切换“通话 APM 模式”。
 * enabled=1：创建 WebRTC APM，开启 AEC3 + 默认高通/降噪，并开始接收 playback reference。
 * enabled=0：关闭 APM，恢复监控 RTSP 使用的原始 PCM 直通。
 * 返回 0 只表示请求已登记；实际重建失败会保留旧 APM 并打印错误日志。
 * 未启动时直接修改下次 start() 使用的配置。不能从上行 packet callback 中调用。
 */
int audio_capture_manager_request_echo_cancellation(AudioCaptureManager *manager, int enabled);

/*
 * 投递即将送本地扬声器播放的 10ms PCM reference。仅在 AEC3 启用时有意义。
 * frame 的 sampleRate/channels/sampleFormat 和帧数必须与当前采集的单个 10ms 周期一致。
 * 本函数可以从播放线程调用；不等待。队列满时淘汰最旧 reference 并仍返回 0。
 * AEC3 未启用时它是成功的无操作：播放链路可以始终调用本接口，不需要与 APM
 * 重建时机竞争，也不会让普通播放白白复制 PCM。
 */
int audio_capture_manager_push_playback_reference(AudioCaptureManager *manager,
                                                  const AudioPcmFrame *frame);

void audio_capture_manager_close(AudioCaptureManager *manager);

#endif
