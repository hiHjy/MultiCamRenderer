#ifndef MCR_AAC_DECODER_INTERNAL_H
#define MCR_AAC_DECODER_INTERNAL_H

#include "AudioDecoder.h"

int audio_aac_decoder_create(AudioDecoder *owner,
                             const AudioPcmFormat *outputFormat,
                             void **implementation,
                             const AudioDecoderOps **ops);

#endif
