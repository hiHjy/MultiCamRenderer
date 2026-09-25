#ifndef MCR_AUDIO_DECODER_H
#define MCR_AUDIO_DECODER_H

#include "AudioTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*AudioDecodedPcmCallback)(const AudioPcmFrame *frame, void *userData);

typedef struct AudioDecoderOps {
    int (*decodePacket)(void *implementation, const AudioEncodedPacket *packet);
    void (*close)(void *implementation);
} AudioDecoderOps;

typedef struct AudioDecoder {
    AudioCodec codec;
    void *implementation;
    const AudioDecoderOps *ops;
    AudioDecodedPcmCallback callback;
    void *callbackUserData;
} AudioDecoder;

void audio_decoder_set_pcm_callback(AudioDecoder *decoder,
                                    AudioDecodedPcmCallback callback,
                                    void *userData);
int audio_decoder_init(AudioDecoder *decoder,
                       AudioCodec codec,
                       const AudioPcmFormat *outputFormat);
int audio_decoder_decode_packet(AudioDecoder *decoder, const AudioEncodedPacket *packet);
void audio_decoder_close(AudioDecoder *decoder);

#ifdef __cplusplus
}
#endif

#endif
