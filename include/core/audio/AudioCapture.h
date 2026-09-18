#ifndef MCR_AUDIO_CAPTURE_H
#define MCR_AUDIO_CAPTURE_H

#include "AudioTypes.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef struct AudioDeviceInfo {
    char alsaName[64];
    char cardId[64];
    char cardName[128];
    char pcmName[128];
    int cardIndex;
    int deviceIndex;
} AudioDeviceInfo;

/* 从 ALSA 物理设备中选择第一个可采集 PCM，不依赖易变化的固定 card 编号。 */
int audio_capture_discover_default_device(AudioDeviceInfo *deviceInfo);

typedef void (*AudioPcmCallback)(const AudioPcmFrame *frame, void *userData);

typedef struct AudioCaptureConfig {
    uint32_t requestedSampleRate;
    uint16_t requestedChannels;
    AudioSampleFormat requestedFormat;
    snd_pcm_uframes_t requestedPeriodFrames;
} AudioCaptureConfig;

typedef struct AudioCapture {
    snd_pcm_t *pcmHandle;
    pthread_t thread;
    atomic_bool stopRequested;
    atomic_bool running;
    AudioPcmCallback callback;
    void *callbackUserData;
    AudioDeviceInfo deviceInfo;
    AudioPcmFormat actualFormat;
    snd_pcm_uframes_t periodFrames;
    uint8_t *buffer;
    size_t bufferBytes;
    uint64_t nextTimestampUs;
    uint64_t timestampRemainder;
    int clockInitialized;
    int threadCreated;
} AudioCapture;

void audio_capture_config_init(AudioCaptureConfig *config);
void audio_capture_set_callback(AudioCapture *capture, AudioPcmCallback callback, void *userData);
int audio_capture_open_auto(AudioCapture *capture, const AudioCaptureConfig *config);
int audio_capture_start(AudioCapture *capture);
void audio_capture_stop(AudioCapture *capture);
void audio_capture_close(AudioCapture *capture);

#endif
