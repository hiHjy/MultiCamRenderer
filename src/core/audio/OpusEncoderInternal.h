#ifndef MCR_OPUS_ENCODER_INTERNAL_H
#define MCR_OPUS_ENCODER_INTERNAL_H

#include "AudioCodec.h"

int audio_opus_encoder_create(AudioEncoder *owner,
                              const AudioEncoderConfig *config,
                              const AudioPcmFormat *inputFormat,
                              void **implementation,
                              const AudioEncoderOps **ops);

#endif
