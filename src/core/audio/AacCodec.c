#include "AudioAac.h"

#include <errno.h>

static int sample_rate_index(uint32_t sampleRate)
{
    static const uint32_t kSampleRates[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350,
    };

    for (size_t index = 0; index < sizeof(kSampleRates) / sizeof(kSampleRates[0]); ++index) {
        if (kSampleRates[index] == sampleRate) {
            return (int)index;
        }
    }
    return -1;
}

int audio_aac_lc_make_audio_specific_config(const AudioPcmFormat *format,
                                             uint8_t *config,
                                             size_t *configSize)
{
    const int frequencyIndex = format == NULL ? -1 : sample_rate_index(format->sampleRate);
    uint16_t packed;

    if (format == NULL || config == NULL || configSize == NULL || *configSize < 2
        || frequencyIndex < 0 || format->channels == 0 || format->channels > 2
        || format->sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE) {
        return -EINVAL;
    }

    /* audioObjectType=2(AAC-LC), frequencyIndex=4 bits, channelConfiguration=4 bits. */
    packed = (uint16_t)((2U << 11) | ((uint16_t)frequencyIndex << 7)
                        | ((uint16_t)format->channels << 3));
    config[0] = (uint8_t)(packed >> 8);
    config[1] = (uint8_t)packed;
    *configSize = 2;
    return 0;
}
