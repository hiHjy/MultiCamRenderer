#ifndef MCR_AUDIO_INPUT_MANAGER_H
#define MCR_AUDIO_INPUT_MANAGER_H

#include "AudioCapture.h"
#include "AudioCodec.h"
#include "AudioApm.h"

typedef struct AudioInputManagerConfig {
    AudioCaptureConfig capture;
    AudioApmConfig apm;
    AudioEncoderConfig encoder;
} AudioInputManagerConfig;

typedef struct AudioInputManager {
    AudioCapture capture;
    AudioApm apm;
    AudioEncoder encoder;
    AudioInputManagerConfig config;
    AudioEncodedPacketCallback callback;
    void *callbackUserData;
    int initialized;
} AudioInputManager;

void audio_input_manager_config_init(AudioInputManagerConfig *config);
int audio_input_manager_init(AudioInputManager *manager,
                             const AudioInputManagerConfig *config);
void audio_input_manager_set_packet_callback(AudioInputManager *manager,
                                             AudioEncodedPacketCallback callback,
                                             void *userData);
int audio_input_manager_start(AudioInputManager *manager);
void audio_input_manager_stop(AudioInputManager *manager);
void audio_input_manager_close(AudioInputManager *manager);

#endif
