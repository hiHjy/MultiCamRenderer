#ifndef MCR_AUDIO_CODEC_H
#define MCR_AUDIO_CODEC_H

#include "AudioTypes.h"

#include <stddef.h>

typedef int (*AudioEncodedPacketCallback)(const AudioEncodedPacket *packet, void *userData);

typedef struct AudioEncoderConfig {
    AudioCodec codec;
    uint32_t bitrate;
    uint32_t frameDurationUs;
    size_t maxPacketBytes;
} AudioEncoderConfig;

typedef struct AudioEncoderOps {
    int (*pushPcm)(void *implementation, const AudioPcmFrame *frame);
    int (*flush)(void *implementation);
    void (*close)(void *implementation);
} AudioEncoderOps;

typedef struct AudioEncoder {
    AudioCodec codec;
    void *implementation;
    const AudioEncoderOps *ops;
    AudioEncodedPacketCallback callback;
    void *callbackUserData;
} AudioEncoder;

void audio_encoder_config_init(AudioEncoderConfig *config);
void audio_encoder_set_packet_callback(AudioEncoder *encoder,
                                       AudioEncodedPacketCallback callback,
                                       void *userData);
int audio_encoder_init(AudioEncoder *encoder,
                       const AudioEncoderConfig *config,
                       const AudioPcmFormat *inputFormat);
int audio_encoder_push_pcm(AudioEncoder *encoder, const AudioPcmFrame *frame);
int audio_encoder_flush(AudioEncoder *encoder);
void audio_encoder_close(AudioEncoder *encoder);

#endif
