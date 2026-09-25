#include "AudioDecoder.h"

#include "AacDecoderInternal.h"
#include "OpusDecoderInternal.h"

#include <errno.h>

void audio_decoder_set_pcm_callback(AudioDecoder *decoder,
                                    AudioDecodedPcmCallback callback,
                                    void *userData) {
    if (decoder == NULL) {
        return;
    }
    decoder->callback = callback;
    decoder->callbackUserData = userData;
}

int audio_decoder_init(AudioDecoder *decoder,
                       AudioCodec codec,
                       const AudioPcmFormat *outputFormat) {
    void *implementation = NULL;
    const AudioDecoderOps *ops = NULL;
    int result;

    if (decoder == NULL || outputFormat == NULL || outputFormat->sampleRate == 0 ||
        outputFormat->channels == 0) {
        return -EINVAL;
    }

    audio_decoder_close(decoder);
    switch (codec) {
    case AUDIO_CODEC_OPUS:
        result = audio_opus_decoder_create(decoder, outputFormat, &implementation, &ops);
        break;
    case AUDIO_CODEC_AAC:
        result = audio_aac_decoder_create(decoder, outputFormat, &implementation, &ops);
        break;
    case AUDIO_CODEC_UNKNOWN:
    case AUDIO_CODEC_G711A:
    case AUDIO_CODEC_G711U:
    default:
        return -ENOTSUP;
    }
    if (result < 0) {
        return result;
    }

    decoder->codec = codec;
    decoder->implementation = implementation;
    decoder->ops = ops;
    return 0;
}

int audio_decoder_decode_packet(AudioDecoder *decoder, const AudioEncodedPacket *packet) {
    if (decoder == NULL || packet == NULL || packet->data == NULL || packet->size == 0 ||
        decoder->implementation == NULL || decoder->ops == NULL || decoder->ops->decodePacket == NULL ||
        decoder->codec != packet->codec) {
        return -EINVAL;
    }
    return decoder->ops->decodePacket(decoder->implementation, packet);
}

void audio_decoder_close(AudioDecoder *decoder) {
    if (decoder == NULL) {
        return;
    }
    if (decoder->implementation != NULL && decoder->ops != NULL && decoder->ops->close != NULL) {
        decoder->ops->close(decoder->implementation);
    }
    decoder->codec = AUDIO_CODEC_UNKNOWN;
    decoder->implementation = NULL;
    decoder->ops = NULL;
}
