#include "AudioCodec.h"

#include "AacEncoderInternal.h"
#include "OpusEncoderInternal.h"

#include <errno.h>
#include <string.h>

void audio_encoder_config_init(AudioEncoderConfig *config) {
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->codec = AUDIO_CODEC_OPUS;
    config->bitrate = 32000;
    config->frameDurationUs = 20000;
    config->frameSamples = 0;
    config->maxPacketBytes = 1200;
}

void audio_encoder_set_packet_callback(AudioEncoder *encoder,
                                       AudioEncodedPacketCallback callback,
                                       void *userData) {
    if (encoder == NULL) {
        return;
    }

    encoder->callback = callback;
    encoder->callbackUserData = userData;
}

int audio_encoder_init(AudioEncoder *encoder,
                       const AudioEncoderConfig *config,
                       const AudioPcmFormat *inputFormat) {
    void *implementation = NULL;
    const AudioEncoderOps *ops = NULL;
    int result;

    if (encoder == NULL || config == NULL || inputFormat == NULL ||
        inputFormat->sampleRate == 0 || inputFormat->channels == 0) {
        return -EINVAL;
    }

    /* callback 属于上层订阅关系；重新初始化编码器时不应丢失。 */
    audio_encoder_close(encoder);

    switch (config->codec) {
    case AUDIO_CODEC_OPUS:
        result = audio_opus_encoder_create(encoder, config, inputFormat,
                                           &implementation, &ops);
        break;
    case AUDIO_CODEC_AAC:
        result = audio_aac_encoder_create(encoder, config, inputFormat,
                                          &implementation, &ops);
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

    encoder->codec = config->codec;
    encoder->implementation = implementation;
    encoder->ops = ops;
    return 0;
}

int audio_encoder_push_pcm(AudioEncoder *encoder, const AudioPcmFrame *frame) {
    if (encoder == NULL || frame == NULL || frame->data == NULL || frame->frames == 0 ||
        encoder->implementation == NULL || encoder->ops == NULL || encoder->ops->pushPcm == NULL) {
        return -EINVAL;
    }
    return encoder->ops->pushPcm(encoder->implementation, frame);
}

int audio_encoder_flush(AudioEncoder *encoder) {
    if (encoder == NULL || encoder->implementation == NULL || encoder->ops == NULL ||
        encoder->ops->flush == NULL) {
        return 0;
    }
    return encoder->ops->flush(encoder->implementation);
}

void audio_encoder_close(AudioEncoder *encoder) {
    if (encoder == NULL) {
        return;
    }

    if (encoder->implementation != NULL && encoder->ops != NULL && encoder->ops->close != NULL) {
        encoder->ops->close(encoder->implementation);
    }
    encoder->codec = AUDIO_CODEC_UNKNOWN;
    encoder->implementation = NULL;
    encoder->ops = NULL;
}
