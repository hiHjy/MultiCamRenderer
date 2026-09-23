#include "AacDecoderInternal.h"

#include "AudioAac.h"

#include <aacdecoder_lib.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct AudioAacDecoder {
    AudioDecoder *owner;
    AudioPcmFormat outputFormat;
    HANDLE_AACDECODER decoder;
    INT_PCM *pcmBuffer;
    size_t pcmBufferSamples;
} AudioAacDecoder;

static int aac_decode_packet(void *implementation, const AudioEncodedPacket *packet)
{
    AudioAacDecoder *decoder = (AudioAacDecoder *)implementation;
    UCHAR *inputData;
    UINT inputSize;
    UINT bytesValid;
    AAC_DECODER_ERROR result;
    CStreamInfo *streamInfo;
    AudioPcmFrame pcm = { 0 };

    if (decoder == NULL || packet == NULL || packet->data == NULL || packet->size == 0
        || packet->codec != AUDIO_CODEC_AAC || packet->sourceFormat.sampleRate != decoder->outputFormat.sampleRate
        || packet->sourceFormat.channels != decoder->outputFormat.channels) {
        return -EINVAL;
    }

    inputData = (UCHAR *)packet->data;
    inputSize = (UINT)packet->size;
    bytesValid = inputSize;
    if (aacDecoder_Fill(decoder->decoder, &inputData, &inputSize, &bytesValid) != AAC_DEC_OK
        || bytesValid != 0) {
        return -EIO;
    }
    result = aacDecoder_DecodeFrame(decoder->decoder, decoder->pcmBuffer,
                                    (INT)decoder->pcmBufferSamples, 0);
    if (!IS_OUTPUT_VALID(result)) {
        return -EIO;
    }

    streamInfo = aacDecoder_GetStreamInfo(decoder->decoder);
    if (streamInfo == NULL || streamInfo->frameSize <= 0 || streamInfo->numChannels != decoder->outputFormat.channels
        || streamInfo->sampleRate != (INT)decoder->outputFormat.sampleRate) {
        return -EIO;
    }
    pcm.data = (const uint8_t *)decoder->pcmBuffer;
    pcm.frames = (size_t)streamInfo->frameSize;
    pcm.format = decoder->outputFormat;
    pcm.timestampUs = packet->timestampUs;
    return decoder->owner->callback == NULL ? 0 : decoder->owner->callback(&pcm, decoder->owner->callbackUserData);
}

static void aac_decoder_close(void *implementation)
{
    AudioAacDecoder *decoder = (AudioAacDecoder *)implementation;

    if (decoder == NULL) {
        return;
    }
    if (decoder->decoder != NULL) {
        aacDecoder_Close(decoder->decoder);
    }
    free(decoder->pcmBuffer);
    free(decoder);
}

static const AudioDecoderOps kAacDecoderOps = {
    .decodePacket = aac_decode_packet,
    .close = aac_decoder_close,
};

int audio_aac_decoder_create(AudioDecoder *owner,
                             const AudioPcmFormat *outputFormat,
                             void **implementation,
                             const AudioDecoderOps **ops)
{
    AudioAacDecoder *decoder;
    uint8_t audioSpecificConfig[2];
    size_t configSize = sizeof(audioSpecificConfig);
    UCHAR *configPointer = audioSpecificConfig;
    UINT configLength;

    if (owner == NULL || outputFormat == NULL || implementation == NULL || ops == NULL
        || audio_aac_lc_make_audio_specific_config(outputFormat, audioSpecificConfig, &configSize) < 0) {
        return -EINVAL;
    }
    decoder = (AudioAacDecoder *)calloc(1, sizeof(*decoder));
    if (decoder == NULL) {
        return -ENOMEM;
    }
    decoder->owner = owner;
    decoder->outputFormat = *outputFormat;
    decoder->pcmBufferSamples = AUDIO_AAC_LC_FRAME_SAMPLES * outputFormat->channels;
    decoder->pcmBuffer = (INT_PCM *)calloc(decoder->pcmBufferSamples, sizeof(INT_PCM));
    decoder->decoder = aacDecoder_Open(TT_MP4_RAW, 1);
    configLength = (UINT)configSize;
    if (decoder->pcmBuffer == NULL || decoder->decoder == NULL
        || aacDecoder_ConfigRaw(decoder->decoder, &configPointer, &configLength) != AAC_DEC_OK) {
        aac_decoder_close(decoder);
        return -EINVAL;
    }

    *implementation = decoder;
    *ops = &kAacDecoderOps;
    return 0;
}
