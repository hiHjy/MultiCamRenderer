#ifndef MCR_AUDIO_AAC_H
#define MCR_AUDIO_AAC_H

#include "AudioTypes.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AAC-LC 标准 access unit 固定包含的每声道 sample 数。 */
enum { AUDIO_AAC_LC_FRAME_SAMPLES = 1024 };

/*
 * 生成 RFC 3640 / RTP MPEG4-GENERIC 所需的两字节 AudioSpecificConfig。
 *
 * 第一版固定 AAC-LC，只接受交错 S16_LE 的 mono 或 stereo。调用方提供至少 2 bytes 的
 * buffer；成功后 *configSize 为 2。Live555 创建 AAC subsession 时可直接使用此配置。
 */
int audio_aac_lc_make_audio_specific_config(const AudioPcmFormat *format,
                                            uint8_t *config,
                                            size_t *configSize);

#ifdef __cplusplus
}
#endif

#endif
