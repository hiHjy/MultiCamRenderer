#include "AudioPlayback.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static AudioSampleFormat sample_format_from_alsa(snd_pcm_format_t format) {
    return format == SND_PCM_FORMAT_S16_LE ? AUDIO_SAMPLE_FORMAT_S16_LE
                                           : AUDIO_SAMPLE_FORMAT_UNKNOWN;
}

static snd_pcm_format_t sample_format_to_alsa(AudioSampleFormat format) {
    return format == AUDIO_SAMPLE_FORMAT_S16_LE ? SND_PCM_FORMAT_S16_LE
                                                : SND_PCM_FORMAT_UNKNOWN;
}

int audio_playback_discover_default_device(AudioDeviceInfo *deviceInfo) {
    int card = -1;

    if (deviceInfo == NULL) {
        return -EINVAL;
    }
    memset(deviceInfo, 0, sizeof(*deviceInfo));
    deviceInfo->cardIndex = -1;
    deviceInfo->deviceIndex = -1;

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
            snd_pcm_info_set_device(pcmInfo, (unsigned int)device);
            snd_pcm_info_set_subdevice(pcmInfo, 0);
            snd_pcm_info_set_stream(pcmInfo, SND_PCM_STREAM_PLAYBACK);
            if (snd_ctl_pcm_info(control, pcmInfo) < 0) {
                continue;
            }

            snprintf(deviceInfo->alsaName, sizeof(deviceInfo->alsaName), "plughw:%d,%d", card, device);
            snprintf(deviceInfo->cardId, sizeof(deviceInfo->cardId), "%s",
                     snd_ctl_card_info_get_id(cardInfo));
            snprintf(deviceInfo->cardName, sizeof(deviceInfo->cardName), "%s",
                     snd_ctl_card_info_get_name(cardInfo));
            snprintf(deviceInfo->pcmName, sizeof(deviceInfo->pcmName), "%s",
                     snd_pcm_info_get_name(pcmInfo));
            deviceInfo->cardIndex = card;
            deviceInfo->deviceIndex = device;
            snd_ctl_close(control);
            return 0;
        }
        snd_ctl_close(control);
    }
    return -ENODEV;
}

void audio_playback_config_init(AudioPlaybackConfig *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->requestedFormat.sampleRate = 48000;
    config->requestedFormat.channels = 1;
    config->requestedFormat.sampleFormat = AUDIO_SAMPLE_FORMAT_S16_LE;
    config->requestedPeriodFrames = 960;
}

int audio_playback_open_auto(AudioPlayback *playback, const AudioPlaybackConfig *config) {
    AudioPlaybackConfig effectiveConfig;
    snd_pcm_hw_params_t *hardwareParams = NULL;
    snd_pcm_format_t alsaFormat;
    unsigned int sampleRate;
    unsigned int channels;
    snd_pcm_uframes_t periodFrames;
    snd_pcm_uframes_t bufferFrames;
    int direction = 0;
    int result;

    if (playback == NULL) {
        return -EINVAL;
    }
    memset(playback, 0, sizeof(*playback));
    audio_playback_config_init(&effectiveConfig);
    if (config != NULL) {
        effectiveConfig = *config;
    }
    alsaFormat = sample_format_to_alsa(effectiveConfig.requestedFormat.sampleFormat);
    if (alsaFormat == SND_PCM_FORMAT_UNKNOWN || effectiveConfig.requestedFormat.sampleRate == 0 ||
        effectiveConfig.requestedFormat.channels == 0 || effectiveConfig.requestedPeriodFrames == 0) {
        return -EINVAL;
    }

    /*
     * 优先交给板级 ALSA 配置选择播放设备。这样可以正确沿用厂商为 codec、功放
     * 或声音路由准备的 "default" 配置，也避免在 HDMI/USB 声卡同时存在时误选
     * 第一个枚举到的物理 PCM。
     */
    snprintf(playback->deviceInfo.alsaName, sizeof(playback->deviceInfo.alsaName), "default");
    snprintf(playback->deviceInfo.cardId, sizeof(playback->deviceInfo.cardId), "ALSA");
    snprintf(playback->deviceInfo.cardName, sizeof(playback->deviceInfo.cardName), "default");
    snprintf(playback->deviceInfo.pcmName, sizeof(playback->deviceInfo.pcmName), "board default route");
    playback->deviceInfo.cardIndex = -1;
    playback->deviceInfo.deviceIndex = -1;
    result = snd_pcm_open(&playback->pcmHandle,
                          playback->deviceInfo.alsaName,
                          SND_PCM_STREAM_PLAYBACK,
                          0);
    if (result < 0) {
        fprintf(stderr, "AudioPlayback: open %s failed: %s, fallback to physical PCM\n",
                playback->deviceInfo.alsaName, snd_strerror(result));
        result = audio_playback_discover_default_device(&playback->deviceInfo);
        if (result < 0) {
            return result;
        }
        result = snd_pcm_open(&playback->pcmHandle,
                              playback->deviceInfo.alsaName,
                              SND_PCM_STREAM_PLAYBACK,
                              0);
        if (result < 0) {
            fprintf(stderr, "AudioPlayback: fallback open %s failed: %s\n",
                    playback->deviceInfo.alsaName, snd_strerror(result));
            audio_playback_close(playback);
            return result;
        }
    }

    snd_pcm_hw_params_alloca(&hardwareParams);
    sampleRate = effectiveConfig.requestedFormat.sampleRate;
    channels = effectiveConfig.requestedFormat.channels;
    periodFrames = effectiveConfig.requestedPeriodFrames;
    bufferFrames = effectiveConfig.requestedBufferFrames == 0
                       ? periodFrames * 4
                       : effectiveConfig.requestedBufferFrames;
    /* 至少保留两个 period；否则 ALSA 即使接受配置，实际也没有可用的调度余量。 */
    if (bufferFrames < periodFrames * 2) {
        bufferFrames = periodFrames * 2;
    }
    if ((result = snd_pcm_hw_params_any(playback->pcmHandle, hardwareParams)) < 0 ||
        (result = snd_pcm_hw_params_set_access(playback->pcmHandle, hardwareParams,
                                                SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (result = snd_pcm_hw_params_set_format(playback->pcmHandle, hardwareParams, alsaFormat)) < 0 ||
        (result = snd_pcm_hw_params_set_rate_near(playback->pcmHandle, hardwareParams,
                                                  &sampleRate, &direction)) < 0 ||
        (result = snd_pcm_hw_params_set_channels_near(playback->pcmHandle, hardwareParams,
                                                      &channels)) < 0 ||
        (result = snd_pcm_hw_params_set_period_size_near(playback->pcmHandle, hardwareParams,
                                                         &periodFrames, &direction)) < 0 ||
        (result = snd_pcm_hw_params_set_buffer_size_near(playback->pcmHandle, hardwareParams,
                                                         &bufferFrames)) < 0 ||
        (result = snd_pcm_hw_params(playback->pcmHandle, hardwareParams)) < 0 ||
        (result = snd_pcm_prepare(playback->pcmHandle)) < 0) {
        fprintf(stderr, "AudioPlayback: configure %s failed: %s\n",
                playback->deviceInfo.alsaName, snd_strerror(result));
        audio_playback_close(playback);
        return result;
    }

    playback->actualFormat.sampleRate = sampleRate;
    playback->actualFormat.channels = (uint16_t)channels;
    playback->actualFormat.sampleFormat = sample_format_from_alsa(alsaFormat);
    playback->periodFrames = periodFrames;
    playback->conversionBufferFrames = periodFrames > 5760 ? periodFrames : 5760;
    playback->conversionBuffer = (int16_t *)calloc(playback->conversionBufferFrames * channels,
                                                    sizeof(int16_t));
    if (playback->conversionBuffer == NULL) {
        audio_playback_close(playback);
        return -ENOMEM;
    }
    fprintf(stdout,
            "AudioPlayback: selected %s [%s / %s], PCM=%uHz %uch S16_LE period=%lu buffer=%lu\n",
            playback->deviceInfo.alsaName,
            playback->deviceInfo.cardId,
            playback->deviceInfo.pcmName,
            playback->actualFormat.sampleRate,
            playback->actualFormat.channels,
            (unsigned long)playback->periodFrames,
            (unsigned long)bufferFrames);
    return 0;
}

int audio_playback_write_pcm(AudioPlayback *playback, const AudioPcmFrame *frame) {
    const int16_t *samples;
    const int16_t *playbackSamples;
    size_t writtenFrames = 0;

    if (playback == NULL || frame == NULL || frame->data == NULL || frame->frames == 0 ||
        playback->pcmHandle == NULL || playback->conversionBuffer == NULL ||
        frame->format.sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE ||
        frame->format.sampleRate != playback->actualFormat.sampleRate ||
        frame->frames > playback->conversionBufferFrames) {
        return -EINVAL;
    }

    samples = (const int16_t *)frame->data;
    playbackSamples = samples;
    if (frame->format.channels == 1 && playback->actualFormat.channels == 2) {
        for (size_t index = 0; index < frame->frames; ++index) {
            playback->conversionBuffer[index * 2] = samples[index];
            playback->conversionBuffer[index * 2 + 1] = samples[index];
        }
        playbackSamples = playback->conversionBuffer;
    } else if (frame->format.channels == 2 && playback->actualFormat.channels == 1) {
        for (size_t index = 0; index < frame->frames; ++index) {
            const int32_t mixed = (int32_t)samples[index * 2] + samples[index * 2 + 1];
            playback->conversionBuffer[index] = (int16_t)(mixed / 2);
        }
        playbackSamples = playback->conversionBuffer;
    } else if (frame->format.channels != playback->actualFormat.channels) {
        return -ENOTSUP;
    }

    while (writtenFrames < frame->frames) {
        const snd_pcm_sframes_t result = snd_pcm_writei(
            playback->pcmHandle,
            playbackSamples + writtenFrames * playback->actualFormat.channels,
            frame->frames - writtenFrames);
        if (result == -EPIPE) {
            fprintf(stderr, "AudioPlayback: ALSA playback XRUN, recovering\n");
            snd_pcm_prepare(playback->pcmHandle);
            continue;
        }
        if (result < 0) {
            const int recovered = snd_pcm_recover(playback->pcmHandle, (int)result, 0);
            if (recovered < 0) {
                fprintf(stderr, "AudioPlayback: write failed permanently: %s\n", snd_strerror(recovered));
                return recovered;
            }
            continue;
        }
        if (result == 0) {
            continue;
        }
        writtenFrames += (size_t)result;
    }
    return 0;
}

void audio_playback_close(AudioPlayback *playback) {
    if (playback == NULL) {
        return;
    }
    if (playback->pcmHandle != NULL) {
        snd_pcm_drain(playback->pcmHandle);
        snd_pcm_close(playback->pcmHandle);
    }
    free(playback->conversionBuffer);
    memset(playback, 0, sizeof(*playback));
}
