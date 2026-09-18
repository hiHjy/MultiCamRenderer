#ifndef MCR_AUDIO_TYPES_H
#define MCR_AUDIO_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* 当前只实现 Opus；枚举先保留后续扩展的稳定编号。 */
typedef enum AudioCodec {
    AUDIO_CODEC_UNKNOWN = 0,
    AUDIO_CODEC_OPUS,
    AUDIO_CODEC_G711A,
    AUDIO_CODEC_G711U,
    AUDIO_CODEC_AAC,
} AudioCodec;

/* 第一版只接收 Opus 所需的交错 S16_LE PCM。 */
typedef enum AudioSampleFormat {
    AUDIO_SAMPLE_FORMAT_UNKNOWN = 0,
    AUDIO_SAMPLE_FORMAT_S16_LE,
} AudioSampleFormat;

typedef struct AudioPcmFormat {
    uint32_t sampleRate;
    uint16_t channels;
    AudioSampleFormat sampleFormat;
} AudioPcmFormat;

/* data 在 PCM 回调返回后失效；消费者若异步处理必须复制。 */
typedef struct AudioPcmFrame {
    const uint8_t *data;
    size_t frames;
    AudioPcmFormat format;
    uint64_t timestampUs;
} AudioPcmFrame;

/* data 在编码包回调返回后失效；RTSP 队列应在回调内复制。 */
typedef struct AudioEncodedPacket {
    AudioCodec codec;
    const uint8_t *data;
    size_t size;
    uint64_t timestampUs;
    uint32_t durationUs;
    AudioPcmFormat sourceFormat;
} AudioEncodedPacket;

#endif
