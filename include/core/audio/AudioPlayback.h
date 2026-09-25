#ifndef MCR_AUDIO_PLAYBACK_H
#define MCR_AUDIO_PLAYBACK_H

#include "AudioCapture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioPlaybackConfig {
    AudioPcmFormat requestedFormat;
    snd_pcm_uframes_t requestedPeriodFrames;

    /*
     * 期望的 ALSA 总硬件缓冲深度，单位 frame。0 表示沿用 period * 4 的通用默认值。
     * 这是声卡调度余量，不是网络 jitter buffer；实时 PlaybackManager 用 8 个 10ms
     * period（80ms），避免普通 Linux 调度偶发晚于 40ms 时触发 ALSA XRUN。
     */
    snd_pcm_uframes_t requestedBufferFrames;

    /*
     * ALSA 硬件队列累积到此 frame 数后才真正起播。0 表示自动取
     * "buffer - period"，避免第一小块 PCM 写入就开播而消耗掉软件预填充余量。
     */
    snd_pcm_uframes_t requestedStartThresholdFrames;
} AudioPlaybackConfig;

/* 单次 write 的运行期事件。它不改变 write 成功/失败的返回值，只让上层能区分
   “正常写入成功”和“ALSA 曾断流、底层已 prepare/recover 后写入成功”。 */
typedef struct AudioPlaybackWriteStatus {
    int recoveredFromDiscontinuity;
} AudioPlaybackWriteStatus;

typedef struct AudioPlayback {
    snd_pcm_t *pcmHandle;
    AudioDeviceInfo deviceInfo;
    AudioPcmFormat actualFormat;
    snd_pcm_uframes_t periodFrames;
    int16_t *conversionBuffer;
    size_t conversionBufferFrames;
} AudioPlayback;

/* 当 ALSA "default" 打开失败时，选择第一个可用的物理 playback PCM 作为回退。 */
int audio_playback_discover_default_device(AudioDeviceInfo *deviceInfo);
void audio_playback_config_init(AudioPlaybackConfig *config);
int audio_playback_open_auto(AudioPlayback *playback, const AudioPlaybackConfig *config);
/* 扩展版 write：status 非 NULL 时报告本次是否发生并恢复了 ALSA XRUN/挂起等断流。 */
int audio_playback_write_pcm_ex(AudioPlayback *playback,
                                const AudioPcmFrame *frame,
                                AudioPlaybackWriteStatus *status);
/* 普通调用不关心断流状态时使用此兼容接口。 */
int audio_playback_write_pcm(AudioPlayback *playback, const AudioPcmFrame *frame);
void audio_playback_close(AudioPlayback *playback);

#ifdef __cplusplus
}
#endif

#endif
