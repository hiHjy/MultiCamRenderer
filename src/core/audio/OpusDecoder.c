#include "OpusDecoderInternal.h"

#include <opus.h>

#include <errno.h>
#include <stdlib.h>

enum { kOpusMaximumFrameSamples = 5760 };

typedef struct AudioOpusDecoder {
    AudioDecoder *owner;
    AudioPcmFormat outputFormat;
    OpusDecoder *opus;
    opus_int16 *pcmBuffer;
} AudioOpusDecoder;

static int is_opus_sample_rate(uint32_t sampleRate) {
    return sampleRate == 8000 || sampleRate == 12000 || sampleRate == 16000 ||
           sampleRate == 24000 || sampleRate == 48000;
}

static int opus_decode_packet(void *implementation, const AudioEncodedPacket *packet) {
    AudioOpusDecoder *decoder = (AudioOpusDecoder *)implementation;
    AudioPcmFrame frame;
    const int decodedFrames = opus_decode(decoder->opus,
                                          packet->data,
                                          (opus_int32)packet->size,
                                          decoder->pcmBuffer,
                                          kOpusMaximumFrameSamples,
                                          0);
    if (decodedFrames < 0) {
        return -EIO;
    }

    frame.data = (const uint8_t *)decoder->pcmBuffer;
    frame.frames = (size_t)decodedFrames;
    frame.format = decoder->outputFormat;
    frame.timestampUs = packet->timestampUs;
    return decoder->owner->callback == NULL ? 0
                                             : decoder->owner->callback(&frame,
                                                                        decoder->owner->callbackUserData);
}

static void opus_decoder_close(void *implementation) {
    AudioOpusDecoder *decoder = (AudioOpusDecoder *)implementation;
    if (decoder == NULL) {
        return;
    }
    opus_decoder_destroy(decoder->opus);
    free(decoder->pcmBuffer);
    free(decoder);
}

static const AudioDecoderOps kOpusDecoderOps = {
    .decodePacket = opus_decode_packet,
    .close = opus_decoder_close,
};

int audio_opus_decoder_create(AudioDecoder *owner,
                              const AudioPcmFormat *outputFormat,
                              void **implementation,
                              const AudioDecoderOps **ops) {
    AudioOpusDecoder *decoder;
    int opusError = OPUS_OK;

    if (owner == NULL || outputFormat == NULL || implementation == NULL || ops == NULL ||
        outputFormat->sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE ||
        !is_opus_sample_rate(outputFormat->sampleRate) || outputFormat->channels == 0 ||
        outputFormat->channels > 2) {
        return -EINVAL;
    }
    decoder = (AudioOpusDecoder *)calloc(1, sizeof(*decoder));
    if (decoder == NULL) {
        return -ENOMEM;
    }

    decoder->owner = owner;
    decoder->outputFormat = *outputFormat;
    decoder->opus = opus_decoder_create((opus_int32)outputFormat->sampleRate,
                                        outputFormat->channels,
                                        &opusError);
    decoder->pcmBuffer = (opus_int16 *)calloc((size_t)kOpusMaximumFrameSamples * outputFormat->channels,
                                              sizeof(opus_int16));
    if (decoder->opus == NULL || opusError != OPUS_OK || decoder->pcmBuffer == NULL) {
        opus_decoder_close(decoder);
        return -ENOMEM;
    }

    *implementation = decoder;
    *ops = &kOpusDecoderOps;
    return 0;
}
