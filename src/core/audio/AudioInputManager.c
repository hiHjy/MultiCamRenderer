#include "AudioInputManager.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int forward_encoded_packet(const AudioEncodedPacket *packet, void *userData) {
    AudioInputManager *manager = (AudioInputManager *)userData;
    if (manager == NULL || manager->callback == NULL) {
        return 0;
    }
    return manager->callback(packet, manager->callbackUserData);
}

static void encode_captured_pcm(const AudioPcmFrame *frame, void *userData) {
    AudioInputManager *manager = (AudioInputManager *)userData;
    AudioPcmFrame processedFrame;
    int result;

    if (manager == NULL || frame == NULL) {
        return;
    }
    result = audio_apm_process_capture(&manager->apm, frame, &processedFrame);
    if (result < 0) {
        fprintf(stderr, "AudioInputManager: APM 处理 PCM 失败: %d\n", result);
        return;
    }
    result = audio_encoder_push_pcm(&manager->encoder, &processedFrame);
    if (result < 0) {
        fprintf(stderr, "AudioInputManager: encode PCM failed: %d\n", result);
    }
}

void audio_input_manager_config_init(AudioInputManagerConfig *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    audio_capture_config_init(&config->capture);
    audio_apm_config_init(&config->apm);
    audio_encoder_config_init(&config->encoder);
}

int audio_input_manager_init(AudioInputManager *manager,
                             const AudioInputManagerConfig *config) {
    if (manager == NULL) {
        return -EINVAL;
    }
    memset(manager, 0, sizeof(*manager));
    audio_input_manager_config_init(&manager->config);
    if (config != NULL) {
        manager->config = *config;
    }
    manager->initialized = 1;
    return 0;
}

void audio_input_manager_set_packet_callback(AudioInputManager *manager,
                                             AudioEncodedPacketCallback callback,
                                             void *userData) {
    if (manager == NULL) {
        return;
    }
    manager->callback = callback;
    manager->callbackUserData = userData;
}

int audio_input_manager_start(AudioInputManager *manager) {
    int result;
    if (manager == NULL || !manager->initialized) {
        return -EINVAL;
    }

    result = audio_capture_open_auto(&manager->capture, &manager->config.capture);
    if (result < 0) {
        return result;
    }

    audio_encoder_set_packet_callback(&manager->encoder, forward_encoded_packet, manager);
    result = audio_encoder_init(&manager->encoder,
                                &manager->config.encoder,
                                &manager->capture.actualFormat);
    if (result < 0) {
        audio_capture_close(&manager->capture);
        return result;
    }

    result = audio_apm_open(&manager->apm,
                            &manager->config.apm,
                            manager->capture.actualFormat,
                            manager->capture.periodFrames);
    if (result < 0) {
        audio_encoder_close(&manager->encoder);
        audio_capture_close(&manager->capture);
        return result;
    }

    audio_capture_set_callback(&manager->capture, encode_captured_pcm, manager);
    result = audio_capture_start(&manager->capture);
    if (result < 0) {
        audio_apm_close(&manager->apm);
        audio_encoder_close(&manager->encoder);
        audio_capture_close(&manager->capture);
        return result;
    }
    return 0;
}

void audio_input_manager_stop(AudioInputManager *manager) {
    if (manager == NULL) {
        return;
    }
    audio_capture_stop(&manager->capture);
}

void audio_input_manager_close(AudioInputManager *manager) {
    if (manager == NULL) {
        return;
    }
    audio_capture_close(&manager->capture);
    audio_apm_close(&manager->apm);
    audio_encoder_flush(&manager->encoder);
    audio_encoder_close(&manager->encoder);
    memset(manager, 0, sizeof(*manager));
}
