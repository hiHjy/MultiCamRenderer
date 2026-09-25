#include "OpusEncoderInternal.h"

#include <opus.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct AudioOpusEncoder {
    AudioEncoder *owner;
    AudioPcmFormat inputFormat;
    uint32_t frameDurationUs;
    int frameSamples;
    size_t cachedFrames;
    uint64_t cachedTimestampUs;
    OpusEncoder *opus;
    opus_int16 *pcmCache;
    uint8_t *packetBuffer;
    size_t maxPacketBytes;
} AudioOpusEncoder;

static uint64_t frames_to_us(size_t frames, uint32_t sampleRate) {
    return (uint64_t)frames * 1000000ULL / sampleRate;
}

static int is_opus_sample_rate(uint32_t sampleRate) {
    return sampleRate == 8000 || sampleRate == 12000 || sampleRate == 16000 ||
           sampleRate == 24000 || sampleRate == 48000;
}

static int is_opus_frame_duration(uint32_t durationUs) {
    return durationUs == 2500 || durationUs == 5000 || durationUs == 10000 ||
           durationUs == 20000 || durationUs == 40000 || durationUs == 60000;
}

static int encode_cached_frame(AudioOpusEncoder *encoder) {
    const int encodedBytes = opus_encode(encoder->opus,
                                         encoder->pcmCache,
                                         encoder->frameSamples,
                                         encoder->packetBuffer,
                                         (opus_int32)encoder->maxPacketBytes);
    AudioEncodedPacket packet;
    int callbackResult = 0;

    if (encodedBytes < 0) {
        return -EIO;
    }

    packet.codec = AUDIO_CODEC_OPUS;
    packet.data = encoder->packetBuffer;
    packet.size = (size_t)encodedBytes;
    packet.timestampUs = encoder->cachedTimestampUs;
    packet.frameSamples = (uint32_t)encoder->frameSamples;
    packet.durationUs = encoder->frameDurationUs;
    packet.sourceFormat = encoder->inputFormat;

    if (encoder->owner->callback != NULL) {
        callbackResult = encoder->owner->callback(&packet, encoder->owner->callbackUserData);
    }

    encoder->cachedFrames = 0;
    return callbackResult;
}

static int opus_push_pcm(void *implementation, const AudioPcmFrame *frame) {
    AudioOpusEncoder *encoder = (AudioOpusEncoder *)implementation;
    const opus_int16 *input;
    size_t consumedFrames = 0;

    if (encoder == NULL || frame == NULL || frame->data == NULL ||
        frame->format.sampleRate != encoder->inputFormat.sampleRate ||
        frame->format.channels != encoder->inputFormat.channels ||
        frame->format.sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE) {
        return -EINVAL;
    }

    input = (const opus_int16 *)frame->data;
    while (consumedFrames < frame->frames) {
        const size_t writableFrames = (size_t)encoder->frameSamples - encoder->cachedFrames;
        const size_t remainingFrames = frame->frames - consumedFrames;
        const size_t copyFrames = remainingFrames < writableFrames ? remainingFrames : writableFrames;

        if (encoder->cachedFrames == 0) {
            encoder->cachedTimestampUs = frame->timestampUs +
                                         frames_to_us(consumedFrames, encoder->inputFormat.sampleRate);
        }

        memcpy(encoder->pcmCache + encoder->cachedFrames * encoder->inputFormat.channels,
               input + consumedFrames * encoder->inputFormat.channels,
               copyFrames * encoder->inputFormat.channels * sizeof(opus_int16));
        encoder->cachedFrames += copyFrames;
        consumedFrames += copyFrames;

        if (encoder->cachedFrames == (size_t)encoder->frameSamples) {
            const int result = encode_cached_frame(encoder);
            if (result < 0) {
                return result;
            }
        }
    }
    return 0;
}

static int opus_flush(void *implementation) {
    AudioOpusEncoder *encoder = (AudioOpusEncoder *)implementation;
    int result;

    if (encoder == NULL || encoder->cachedFrames == 0) {
        return 0;
    }

    memset(encoder->pcmCache + encoder->cachedFrames * encoder->inputFormat.channels,
           0,
           ((size_t)encoder->frameSamples - encoder->cachedFrames) *
               encoder->inputFormat.channels * sizeof(opus_int16));
    encoder->cachedFrames = (size_t)encoder->frameSamples;
    result = encode_cached_frame(encoder);
    return result;
}

static int opus_reset(void *implementation) {
    AudioOpusEncoder *encoder = (AudioOpusEncoder *)implementation;

    if (encoder == NULL || encoder->opus == NULL) {
        return -EINVAL;
    }
    /* 丢弃未满包 PCM，并将 Opus predictor/entropy state 还原成 freshly-created 状态。
       配置、packetBuffer 与已分配的 PCM cache 全部保留。 */
    encoder->cachedFrames = 0;
    encoder->cachedTimestampUs = 0;
    return opus_encoder_ctl(encoder->opus, OPUS_RESET_STATE) == OPUS_OK ? 0 : -EIO;
}

static void opus_close(void *implementation) {
    AudioOpusEncoder *encoder = (AudioOpusEncoder *)implementation;
    if (encoder == NULL) {
        return;
    }
    opus_encoder_destroy(encoder->opus);
    free(encoder->pcmCache);
    free(encoder->packetBuffer);
    free(encoder);
}

static const AudioEncoderOps kOpusEncoderOps = {
    .pushPcm = opus_push_pcm,
    .flush = opus_flush,
    .reset = opus_reset,
    .close = opus_close,
};

int audio_opus_encoder_create(AudioEncoder *owner,
                              const AudioEncoderConfig *config,
                              const AudioPcmFormat *inputFormat,
                              void **implementation,
                              const AudioEncoderOps **ops) {
    AudioOpusEncoder *encoder;
    int opusError = OPUS_OK;

    if (owner == NULL || config == NULL || inputFormat == NULL || implementation == NULL || ops == NULL ||
        inputFormat->sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE ||
        !is_opus_sample_rate(inputFormat->sampleRate) || inputFormat->channels == 0 ||
        inputFormat->channels > 2 || !is_opus_frame_duration(config->frameDurationUs) ||
        config->maxPacketBytes == 0 || config->maxPacketBytes > INT_MAX ||
        ((uint64_t)inputFormat->sampleRate * config->frameDurationUs) % 1000000ULL != 0) {
        return -EINVAL;
    }

    encoder = (AudioOpusEncoder *)calloc(1, sizeof(*encoder));
    if (encoder == NULL) {
        return -ENOMEM;
    }

    encoder->owner = owner;
    encoder->inputFormat = *inputFormat;
    encoder->frameDurationUs = config->frameDurationUs;
    encoder->frameSamples = (int)((uint64_t)inputFormat->sampleRate * config->frameDurationUs / 1000000ULL);
    encoder->maxPacketBytes = config->maxPacketBytes;
    encoder->opus = opus_encoder_create((opus_int32)inputFormat->sampleRate,
                                        inputFormat->channels,
                                        OPUS_APPLICATION_VOIP,
                                        &opusError);
    if (encoder->opus == NULL || opusError != OPUS_OK) {
        opus_close(encoder);
        return -EINVAL;
    }
    if (opus_encoder_ctl(encoder->opus, OPUS_SET_BITRATE((opus_int32)config->bitrate)) != OPUS_OK) {
        opus_close(encoder);
        return -EIO;
    }

    encoder->pcmCache = (opus_int16 *)calloc((size_t)encoder->frameSamples * inputFormat->channels,
                                             sizeof(opus_int16));
    encoder->packetBuffer = (uint8_t *)malloc(config->maxPacketBytes);
    if (encoder->pcmCache == NULL || encoder->packetBuffer == NULL) {
        opus_close(encoder);
        return -ENOMEM;
    }

    *implementation = encoder;
    *ops = &kOpusEncoderOps;
    return 0;
}
