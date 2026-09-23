#ifndef MCR_AAC_ENCODER_INTERNAL_H
#define MCR_AAC_ENCODER_INTERNAL_H

#include "AudioCodec.h"

int audio_aac_encoder_create(AudioEncoder *owner,
                             const AudioEncoderConfig *config,
                             const AudioPcmFormat *inputFormat,
                             void **implementation,
                             const AudioEncoderOps **ops);

#endif
