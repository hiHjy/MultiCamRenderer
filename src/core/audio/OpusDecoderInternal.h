#ifndef MCR_OPUS_DECODER_INTERNAL_H
#define MCR_OPUS_DECODER_INTERNAL_H

#include "AudioDecoder.h"

int audio_opus_decoder_create(AudioDecoder *owner,
                              const AudioPcmFormat *outputFormat,
                              void **implementation,
                              const AudioDecoderOps **ops);

#endif
