#include "AudioCapture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000ULL + (uint64_t)now.tv_nsec / 1000ULL;
}

static AudioSampleFormat sample_format_from_alsa(snd_pcm_format_t format) {
    return format == SND_PCM_FORMAT_S16_LE ? AUDIO_SAMPLE_FORMAT_S16_LE
                                           : AUDIO_SAMPLE_FORMAT_UNKNOWN;
}

static snd_pcm_format_t sample_format_to_alsa(AudioSampleFormat format) {
    return format == AUDIO_SAMPLE_FORMAT_S16_LE ? SND_PCM_FORMAT_S16_LE
                                                : SND_PCM_FORMAT_UNKNOWN;
}

static void advance_timestamp(AudioCapture *capture, size_t frames) {
    const uint64_t numerator = (uint64_t)frames * 1000000ULL + capture->timestampRemainder;
    capture->nextTimestampUs += numerator / capture->actualFormat.sampleRate;
    capture->timestampRemainder = numerator % capture->actualFormat.sampleRate;
}

static int pcm_supports_stream(snd_ctl_t *control, int device, snd_pcm_stream_t stream) {
    snd_pcm_info_t *pcmInfo = NULL;
    snd_pcm_info_alloca(&pcmInfo);
    snd_pcm_info_set_device(pcmInfo, (unsigned int)device);
    snd_pcm_info_set_subdevice(pcmInfo, 0);
    snd_pcm_info_set_stream(pcmInfo, stream);
    return snd_ctl_pcm_info(control, pcmInfo) >= 0;
}

static void fill_device_info(AudioDeviceInfo *deviceInfo,
                             int card,
                             int device,
                             const snd_ctl_card_info_t *cardInfo,
                             const snd_pcm_info_t *pcmInfo) {
    snprintf(deviceInfo->alsaName, sizeof(deviceInfo->alsaName), "plughw:%d,%d", card, device);
    snprintf(deviceInfo->cardId, sizeof(deviceInfo->cardId), "%s",
             snd_ctl_card_info_get_id(cardInfo));
    snprintf(deviceInfo->cardName, sizeof(deviceInfo->cardName), "%s",
             snd_ctl_card_info_get_name(cardInfo));
    snprintf(deviceInfo->pcmName, sizeof(deviceInfo->pcmName), "%s",
             snd_pcm_info_get_name(pcmInfo));
    deviceInfo->cardIndex = card;
    deviceInfo->deviceIndex = device;
}

int audio_capture_discover_default_device(AudioDeviceInfo *deviceInfo) {
    int card = -1;
    AudioDeviceInfo fallbackDevice;
    int hasFallbackDevice = 0;

    if (deviceInfo == NULL) {
        return -EINVAL;
    }
    memset(deviceInfo, 0, sizeof(*deviceInfo));
    deviceInfo->cardIndex = -1;
    deviceInfo->deviceIndex = -1;
    memset(&fallbackDevice, 0, sizeof(fallbackDevice));

    while (snd_card_next(&card) >= 0 && card >= 0) {
        char controlName[32];
        snd_ctl_t *control = NULL;
        snd_ctl_card_info_t *cardInfo = NULL;
        int device = -1;

        snprintf(controlName, sizeof(controlName), "hw:%d", card);
        if (snd_ctl_open(&control, controlName, 0) < 0) {
            continue;
        }

        snd_ctl_card_info_alloca(&cardInfo);
        if (snd_ctl_card_info(control, cardInfo) < 0) {
            snd_ctl_close(control);
            continue;
        }

        while (snd_ctl_pcm_next_device(control, &device) >= 0 && device >= 0) {
            snd_pcm_info_t *pcmInfo = NULL;
            snd_pcm_info_alloca(&pcmInfo);
            if (!pcm_supports_stream(control, device, SND_PCM_STREAM_CAPTURE)) {
                continue;
            }
            snd_pcm_info_set_device(pcmInfo, (unsigned int)device);
            snd_pcm_info_set_subdevice(pcmInfo, 0);
            snd_pcm_info_set_stream(pcmInfo, SND_PCM_STREAM_CAPTURE);
            if (snd_ctl_pcm_info(control, pcmInfo) < 0) {
                continue;
            }

            /*
             * USB 摄像头常排在 card0，却可能离实际安装位置很远。优先选同时具有
             * playback 的全双工 PCM，通常就是板载 codec；没有时仍能回退到首个
             * capture-only 设备，不把自动发现写死为某张声卡。
             */
            if (pcm_supports_stream(control, device, SND_PCM_STREAM_PLAYBACK)) {
                fill_device_info(deviceInfo, card, device, cardInfo, pcmInfo);
                snd_ctl_close(control);
                return 0;
            }
            if (!hasFallbackDevice) {
                fill_device_info(&fallbackDevice, card, device, cardInfo, pcmInfo);
                hasFallbackDevice = 1;
            }
        }
        snd_ctl_close(control);
    }

    if (hasFallbackDevice) {
        *deviceInfo = fallbackDevice;
        return 0;
    }
    return -ENODEV;
}

void audio_capture_config_init(AudioCaptureConfig *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->requestedSampleRate = 48000;
    config->requestedChannels = 1;
    config->requestedFormat = AUDIO_SAMPLE_FORMAT_S16_LE;
    config->requestedPeriodFrames = 960;
}

void audio_capture_set_callback(AudioCapture *capture, AudioPcmCallback callback, void *userData) {
    if (capture == NULL) {
        return;
    }
    capture->callback = callback;
    capture->callbackUserData = userData;
}

static void *capture_thread_main(void *argument) {
    AudioCapture *capture = (AudioCapture *)argument;

    while (capture != NULL && !atomic_load(&capture->stopRequested)) {
        const snd_pcm_sframes_t frames = snd_pcm_readi(capture->pcmHandle,
                                                        capture->buffer,
                                                        capture->periodFrames);
        // stop() 会用 snd_pcm_drop() 唤醒阻塞的 readi()；不同 ALSA 驱动可能在这里
        // 返回 EPIPE 或 EIO。既然退出标志已置位，这不是设备故障，直接结束线程即可。
        if (atomic_load(&capture->stopRequested)) {
            break;
        }
        if (frames == -EPIPE) {
            fprintf(stderr, "AudioCapture: ALSA capture XRUN, recovering\n");
            snd_pcm_prepare(capture->pcmHandle);
            capture->clockInitialized = 0;
            continue;
        }
        if (frames < 0) {
            const int recovered = snd_pcm_recover(capture->pcmHandle, (int)frames, 0);
            if (recovered < 0) {
                fprintf(stderr, "AudioCapture: read failed permanently: %s\n", snd_strerror(recovered));
                break;
            }
            capture->clockInitialized = 0;
            continue;
        }
        if (frames == 0) {
            continue;
        }

        if (!capture->clockInitialized) {
            const uint64_t blockDurationUs = (uint64_t)frames * 1000000ULL /
                                             capture->actualFormat.sampleRate;
            const uint64_t nowUs = monotonic_now_us();
            capture->nextTimestampUs = nowUs > blockDurationUs ? nowUs - blockDurationUs : 0;
            capture->timestampRemainder = 0;
            capture->clockInitialized = 1;
        }

        if (capture->callback != NULL) {
            const AudioPcmFrame frame = {
                .data = capture->buffer,
                .frames = (size_t)frames,
                .format = capture->actualFormat,
                .timestampUs = capture->nextTimestampUs,
            };
            capture->callback(&frame, capture->callbackUserData);
        }
        advance_timestamp(capture, (size_t)frames);
    }

    if (capture != NULL) {
        atomic_store(&capture->running, false);
    }
    return NULL;
}

int audio_capture_open_auto(AudioCapture *capture, const AudioCaptureConfig *config) {
    AudioCaptureConfig effectiveConfig;
    snd_pcm_hw_params_t *hardwareParams = NULL;
    snd_pcm_format_t alsaFormat;
    unsigned int sampleRate;
    unsigned int channels;
    snd_pcm_uframes_t periodFrames;
    int direction = 0;
    int result;

    if (capture == NULL) {
        return -EINVAL;
    }
    memset(capture, 0, sizeof(*capture));
    atomic_init(&capture->stopRequested, false);
    atomic_init(&capture->running, false);

    audio_capture_config_init(&effectiveConfig);
    if (config != NULL) {
        effectiveConfig = *config;
    }
    alsaFormat = sample_format_to_alsa(effectiveConfig.requestedFormat);
    if (alsaFormat == SND_PCM_FORMAT_UNKNOWN || effectiveConfig.requestedSampleRate == 0 ||
        effectiveConfig.requestedChannels == 0 || effectiveConfig.requestedPeriodFrames == 0) {
        return -EINVAL;
    }

    result = audio_capture_discover_default_device(&capture->deviceInfo);
    if (result < 0) {
        return result;
    }

    result = snd_pcm_open(&capture->pcmHandle,
                          capture->deviceInfo.alsaName,
                          SND_PCM_STREAM_CAPTURE,
                          0);
    if (result < 0) {
        fprintf(stderr, "AudioCapture: open %s failed: %s\n",
                capture->deviceInfo.alsaName, snd_strerror(result));
        audio_capture_close(capture);
        return result;
    }

    snd_pcm_hw_params_alloca(&hardwareParams);
    if ((result = snd_pcm_hw_params_any(capture->pcmHandle, hardwareParams)) < 0 ||
        (result = snd_pcm_hw_params_set_access(capture->pcmHandle, hardwareParams,
                                                SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (result = snd_pcm_hw_params_set_format(capture->pcmHandle, hardwareParams, alsaFormat)) < 0) {
        audio_capture_close(capture);
        return result;
    }

    sampleRate = effectiveConfig.requestedSampleRate;
    channels = effectiveConfig.requestedChannels;
    periodFrames = effectiveConfig.requestedPeriodFrames;
    if ((result = snd_pcm_hw_params_set_rate_near(capture->pcmHandle, hardwareParams,
                                                  &sampleRate, &direction)) < 0 ||
        (result = snd_pcm_hw_params_set_channels_near(capture->pcmHandle, hardwareParams,
                                                      &channels)) < 0 ||
        (result = snd_pcm_hw_params_set_period_size_near(capture->pcmHandle, hardwareParams,
                                                         &periodFrames, &direction)) < 0 ||
        (result = snd_pcm_hw_params(capture->pcmHandle, hardwareParams)) < 0 ||
        (result = snd_pcm_prepare(capture->pcmHandle)) < 0) {
        fprintf(stderr, "AudioCapture: configure %s failed: %s\n",
                capture->deviceInfo.alsaName, snd_strerror(result));
        audio_capture_close(capture);
        return result;
    }

    capture->actualFormat.sampleRate = sampleRate;
    capture->actualFormat.channels = (uint16_t)channels;
    capture->actualFormat.sampleFormat = sample_format_from_alsa(alsaFormat);
    capture->periodFrames = periodFrames;
    capture->bufferBytes = (size_t)periodFrames * channels * sizeof(int16_t);
    capture->buffer = (uint8_t *)calloc(1, capture->bufferBytes);
    if (capture->buffer == NULL) {
        audio_capture_close(capture);
        return -ENOMEM;
    }

    fprintf(stdout,
            "AudioCapture: selected %s [%s / %s], PCM=%uHz %uch S16_LE period=%lu\n",
            capture->deviceInfo.alsaName,
            capture->deviceInfo.cardId,
            capture->deviceInfo.pcmName,
            capture->actualFormat.sampleRate,
            capture->actualFormat.channels,
            (unsigned long)capture->periodFrames);
    return 0;
}

int audio_capture_start(AudioCapture *capture) {
    int result;
    if (capture == NULL || capture->pcmHandle == NULL || capture->buffer == NULL) {
        return -EINVAL;
    }
    if (capture->threadCreated) {
        return -EBUSY;
    }
    atomic_store(&capture->stopRequested, false);
    atomic_store(&capture->running, true);
    result = pthread_create(&capture->thread, NULL, capture_thread_main, capture);
    if (result != 0) {
        atomic_store(&capture->running, false);
        return -result;
    }
    capture->threadCreated = 1;
    return 0;
}

void audio_capture_stop(AudioCapture *capture) {
    if (capture == NULL || !capture->threadCreated) {
        return;
    }
    atomic_store(&capture->stopRequested, true);
    if (capture->pcmHandle != NULL) {
        snd_pcm_drop(capture->pcmHandle);
    }
    pthread_join(capture->thread, NULL);
    capture->threadCreated = 0;
}

void audio_capture_close(AudioCapture *capture) {
    if (capture == NULL) {
        return;
    }
    audio_capture_stop(capture);
    if (capture->pcmHandle != NULL) {
        snd_pcm_close(capture->pcmHandle);
    }
    free(capture->buffer);
    memset(capture, 0, sizeof(*capture));
}
