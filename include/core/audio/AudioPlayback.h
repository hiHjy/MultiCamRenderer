#ifndef MCR_AUDIO_PLAYBACK_H
#define MCR_AUDIO_PLAYBACK_H

#include "AudioCapture.h"

typedef struct AudioPlaybackConfig {
    AudioPcmFormat requestedFormat;
    snd_pcm_uframes_t requestedPeriodFrames;
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
