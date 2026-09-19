#ifndef MCR_AUDIO_PLAYBACK_H
#define MCR_AUDIO_PLAYBACK_H

#include "AudioCapture.h"

typedef struct AudioPlaybackConfig {
    AudioPcmFormat requestedFormat;
    snd_pcm_uframes_t requestedPeriodFrames;

    /*
     * 期望的 ALSA 总硬件缓冲深度，单位 frame。0 表示沿用 period * 4 的通用默认值。
     * 这是声卡调度余量，不是网络 jitter buffer；实时 PlaybackManager 用 8 个 10ms
     * period（80ms），避免普通 Linux 调度偶发晚于 40ms 时触发 ALSA XRUN。
     */
    snd_pcm_uframes_t requestedBufferFrames;
} AudioPlaybackConfig;

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
int audio_playback_write_pcm(AudioPlayback *playback, const AudioPcmFrame *frame);
void audio_playback_close(AudioPlayback *playback);

#endif
