#ifndef MCR_AUDIO_TYPES_H
#define MCR_AUDIO_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* 压缩音频编码类型。枚举值会写入 demo packet 文件，因此新增时只能追加，不能改已有编号。 */
typedef enum AudioCodec {
    AUDIO_CODEC_UNKNOWN = 0,
    AUDIO_CODEC_OPUS,
    AUDIO_CODEC_G711A,
    AUDIO_CODEC_G711U,
    AUDIO_CODEC_AAC,
} AudioCodec;

/* PCM sample 的内存表示。当前音频图只接受交错的 16-bit little-endian PCM。 */
typedef enum AudioSampleFormat {
    AUDIO_SAMPLE_FORMAT_UNKNOWN = 0,
    AUDIO_SAMPLE_FORMAT_S16_LE,
} AudioSampleFormat;

typedef struct AudioPcmFormat {
    /* 每秒每声道的采样个数，例如 48000。 */
    uint32_t sampleRate;
    /* 交错声道数；第一版支持 mono(1) 与 stereo(2)。 */
    uint16_t channels;
    /* samples 的数据类型；当前必须为 AUDIO_SAMPLE_FORMAT_S16_LE。 */
    AudioSampleFormat sampleFormat;
} AudioPcmFormat;

/* PCM 的借用视图；data 在产生此帧的 C 回调返回后失效，异步消费者必须复制。 */
typedef struct AudioPcmFrame {
    /* 交错 PCM 首地址，不拥有内存。 */
    const uint8_t *data;
    /* 每个声道的 sample 数，不是 data 的字节数。 */
    size_t frames;
    /* PCM 的采样率、声道数与数据类型。 */
    AudioPcmFormat format;
    /* 该块 PCM 第一帧所在的 CLOCK_MONOTONIC 微秒时间线。 */
    uint64_t timestampUs;
} AudioPcmFrame;

/* 压缩音频包的借用视图；data 在编码回调返回后失效，异步消费者必须复制。 */
typedef struct AudioEncodedPacket {
    /* 该 access unit 的编码类型。 */
    AudioCodec codec;
    /* 压缩 payload 首地址，不拥有内存。 */
    const uint8_t *data;
    /* 压缩 payload 的字节数。 */
    size_t size;
    /* 本包覆盖音频的第一 sample 在单调时钟中的微秒时间戳。 */
    uint64_t timestampUs;
    /* 本包准确覆盖的每声道 sample 数；AAC-LC 固定为 1024，Opus 常见为 960。 */
    uint32_t frameSamples;
    /* 由 frameSamples/sampleRate 换算的近似微秒时长，仅供旧接口和队列预算使用。
       精确 RTP 时间线必须使用 frameSamples，不能累加此字段。 */
    uint32_t durationUs;
    /* 编码前 PCM 的格式；解码器和 RTP SDP 用它建立采样率/声道契约。 */
    AudioPcmFormat sourceFormat;
} AudioEncodedPacket;

#endif
