#include "AacEncoderInternal.h"

#include "AudioAac.h"

#include <aacenc_lib.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct AudioAacEncoder {
    AudioEncoder *owner;
    AudioPcmFormat inputFormat;
    size_t cachedFrames;
    uint64_t cachedTimestampUs;
    HANDLE_AACENCODER encoder;
    INT_PCM *pcmCache;
    uint8_t *packetBuffer;
    size_t maxPacketBytes;
} AudioAacEncoder;

static uint64_t frames_to_us(size_t frames, uint32_t sampleRate)
{
    return (uint64_t)frames * 1000000ULL / sampleRate;
}

static CHANNEL_MODE channel_mode(uint16_t channels)
{
    return channels == 1 ? MODE_1 : channels == 2 ? MODE_2 : MODE_INVALID;
}

static int emit_access_unit(AudioAacEncoder *encoder)
{
    void *inputBuffer = encoder->pcmCache;
    INT inputIdentifier = IN_AUDIO_DATA;
    INT inputSize = AUDIO_AAC_LC_FRAME_SAMPLES * encoder->inputFormat.channels * (INT)sizeof(INT_PCM);
    INT inputElementSize = (INT)sizeof(INT_PCM);
    AACENC_BufDesc inputDescription = {
        .numBufs = 1,
        .bufs = &inputBuffer,
        .bufferIdentifiers = &inputIdentifier,
        .bufSizes = &inputSize,
        .bufElSizes = &inputElementSize,
    };
    void *outputBuffer = encoder->packetBuffer;
    INT outputIdentifier = OUT_BITSTREAM_DATA;
    INT outputSize = (INT)encoder->maxPacketBytes;
    INT outputElementSize = 1;
    AACENC_BufDesc outputDescription = {
        .numBufs = 1,
        .bufs = &outputBuffer,
        .bufferIdentifiers = &outputIdentifier,
        .bufSizes = &outputSize,
        .bufElSizes = &outputElementSize,
    };
    AACENC_InArgs inputArguments = { 0 };
    AACENC_OutArgs outputArguments = { 0 };
    AudioEncodedPacket packet = { 0 };
    AACENC_ERROR result;

    inputArguments.numInSamples = (INT)(AUDIO_AAC_LC_FRAME_SAMPLES * encoder->inputFormat.channels);
    result = aacEncEncode(encoder->encoder, &inputDescription, &outputDescription,
                          &inputArguments, &outputArguments);
    if (result != AACENC_OK) {
        return -EIO;
    }
    /* FDK 在首次编码或刚 reset history 后可能已消费 1024 samples、但暂未输出 access
       unit。无论是否有输出，这组输入都已归属底层，应用层 cache 必须释放；否则下一次
       writableFrames 会变成 0，aac_push_pcm() 会原地空转。 */
    encoder->cachedFrames = 0;
    if (outputArguments.numOutBytes <= 0) {
        return 0;
    }

    packet.codec = AUDIO_CODEC_AAC;
    packet.data = encoder->packetBuffer;
    packet.size = (size_t)outputArguments.numOutBytes;
    packet.timestampUs = encoder->cachedTimestampUs;
    packet.frameSamples = AUDIO_AAC_LC_FRAME_SAMPLES;
    packet.durationUs = (uint32_t)frames_to_us(AUDIO_AAC_LC_FRAME_SAMPLES,
                                                encoder->inputFormat.sampleRate);
    packet.sourceFormat = encoder->inputFormat;

    return encoder->owner->callback == NULL
        ? 0
        : encoder->owner->callback(&packet, encoder->owner->callbackUserData);
}

static int aac_push_pcm(void *implementation, const AudioPcmFrame *frame)
{
    AudioAacEncoder *encoder = (AudioAacEncoder *)implementation;
    const INT_PCM *input;
    size_t consumedFrames = 0;

    if (encoder == NULL || frame == NULL || frame->data == NULL
        || frame->format.sampleRate != encoder->inputFormat.sampleRate
        || frame->format.channels != encoder->inputFormat.channels
        || frame->format.sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE) {
        return -EINVAL;
    }

    input = (const INT_PCM *)frame->data;
    while (consumedFrames < frame->frames) {
        const size_t writableFrames = AUDIO_AAC_LC_FRAME_SAMPLES - encoder->cachedFrames;
        const size_t remainingFrames = frame->frames - consumedFrames;
        const size_t copyFrames = remainingFrames < writableFrames ? remainingFrames : writableFrames;

        if (encoder->cachedFrames == 0) {
            encoder->cachedTimestampUs = frame->timestampUs
                + frames_to_us(consumedFrames, encoder->inputFormat.sampleRate);
        }
        memcpy(encoder->pcmCache + encoder->cachedFrames * encoder->inputFormat.channels,
               input + consumedFrames * encoder->inputFormat.channels,
               copyFrames * encoder->inputFormat.channels * sizeof(INT_PCM));
        encoder->cachedFrames += copyFrames;
        consumedFrames += copyFrames;

        if (encoder->cachedFrames == AUDIO_AAC_LC_FRAME_SAMPLES) {
            const int result = emit_access_unit(encoder);
            if (result < 0) {
                return result;
            }
        }
    }
    return 0;
}

static int aac_flush(void *implementation)
{
    AudioAacEncoder *encoder = (AudioAacEncoder *)implementation;

    if (encoder == NULL || encoder->cachedFrames == 0) {
        return 0;
    }
    memset(encoder->pcmCache + encoder->cachedFrames * encoder->inputFormat.channels, 0,
           (AUDIO_AAC_LC_FRAME_SAMPLES - encoder->cachedFrames) * encoder->inputFormat.channels
               * sizeof(INT_PCM));
    encoder->cachedFrames = AUDIO_AAC_LC_FRAME_SAMPLES;
    return emit_access_unit(encoder);
}

static int aac_reset(void *implementation)
{
    AudioAacEncoder *encoder = (AudioAacEncoder *)implementation;

    if (encoder == NULL || encoder->encoder == NULL) {
        return -EINVAL;
    }

    /* 先丢弃应用层尚未凑齐的一组 1024 samples，绝不把时间缺口两侧的 PCM 混成一包。 */
    encoder->cachedFrames = 0;
    encoder->cachedTimestampUs = 0;

    /* FDK-AAC 不是只有 pcmCache：psychoacoustic 等模块也持有 history。官方
       CONTROL_STATE 能在不释放 handle 的前提下清所有 history 与内部 input buffer；
       随后的空 encode 立即执行该重置，而不是拖到下一包音频。 */
    if (aacEncoder_SetParam(encoder->encoder, AACENC_CONTROL_STATE,
                            AACENC_INIT_STATES | AACENC_RESET_INBUFFER) != AACENC_OK
        || aacEncEncode(encoder->encoder, NULL, NULL, NULL, NULL) != AACENC_OK) {
        return -EIO;
    }
    return 0;
}

static void aac_close(void *implementation)
{
    AudioAacEncoder *encoder = (AudioAacEncoder *)implementation;

    if (encoder == NULL) {
        return;
    }
    if (encoder->encoder != NULL) {
        aacEncClose(&encoder->encoder);
    }
    free(encoder->pcmCache);
    free(encoder->packetBuffer);
    free(encoder);
}

static const AudioEncoderOps kAacEncoderOps = {
    .pushPcm = aac_push_pcm,
    .flush = aac_flush,
    .reset = aac_reset,
    .close = aac_close,
};

int audio_aac_encoder_create(AudioEncoder *owner,
                             const AudioEncoderConfig *config,
                             const AudioPcmFormat *inputFormat,
                             void **implementation,
                             const AudioEncoderOps **ops)
{
    AudioAacEncoder *encoder;
    AACENC_ERROR result;

    if (owner == NULL || config == NULL || inputFormat == NULL || implementation == NULL || ops == NULL
        || inputFormat->sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE
        || channel_mode(inputFormat->channels) == MODE_INVALID || config->bitrate == 0
        || config->maxPacketBytes == 0 || config->maxPacketBytes > INT_MAX
        || (config->frameSamples != 0 && config->frameSamples != AUDIO_AAC_LC_FRAME_SAMPLES)) {
        return -EINVAL;
    }

    encoder = (AudioAacEncoder *)calloc(1, sizeof(*encoder));
    if (encoder == NULL) {
        return -ENOMEM;
    }
    encoder->owner = owner;
    encoder->inputFormat = *inputFormat;
    encoder->maxPacketBytes = config->maxPacketBytes;

    result = aacEncOpen(&encoder->encoder, 0, inputFormat->channels);
    if (result != AACENC_OK
        || aacEncoder_SetParam(encoder->encoder, AACENC_AOT, AOT_AAC_LC) != AACENC_OK
        || aacEncoder_SetParam(encoder->encoder, AACENC_SAMPLERATE, inputFormat->sampleRate) != AACENC_OK
        || aacEncoder_SetParam(encoder->encoder, AACENC_CHANNELMODE, channel_mode(inputFormat->channels)) != AACENC_OK
        || aacEncoder_SetParam(encoder->encoder, AACENC_BITRATE, config->bitrate) != AACENC_OK
        || aacEncoder_SetParam(encoder->encoder, AACENC_TRANSMUX, TT_MP4_RAW) != AACENC_OK
        /* 板端 A/B：64kbps mono 下 Afterburner 额外约 2% 单核，输出体积几乎相同；
           目前没有证明其听感收益足以抵消常驻计算，因此固定关闭。 */
        || aacEncoder_SetParam(encoder->encoder, AACENC_AFTERBURNER, 0) != AACENC_OK
        || aacEncEncode(encoder->encoder, NULL, NULL, NULL, NULL) != AACENC_OK) {
        aac_close(encoder);
        return -EINVAL;
    }

    encoder->pcmCache = (INT_PCM *)calloc(AUDIO_AAC_LC_FRAME_SAMPLES * inputFormat->channels,
                                          sizeof(INT_PCM));
    encoder->packetBuffer = (uint8_t *)malloc(config->maxPacketBytes);
    if (encoder->pcmCache == NULL || encoder->packetBuffer == NULL) {
        aac_close(encoder);
        return -ENOMEM;
    }

    *implementation = encoder;
    *ops = &kAacEncoderOps;
    return 0;
}
