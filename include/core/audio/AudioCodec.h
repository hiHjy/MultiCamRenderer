#ifndef MCR_AUDIO_CODEC_H
#define MCR_AUDIO_CODEC_H

#include "AudioTypes.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*AudioEncodedPacketCallback)(const AudioEncodedPacket *packet, void *userData);

/* 一个 Encoder Node 的编码策略。实际 PCM 格式来自 AudioFrame，而不是上层重复填写。 */
typedef struct AudioEncoderConfig {
    /* 要创建的压缩编码器，例如 AUDIO_CODEC_OPUS 或 AUDIO_CODEC_AAC。 */
    AudioCodec codec;
    /* 目标平均码率，单位 bit/s。 */
    uint32_t bitrate;
    /* 以微秒表示的请求编码帧时长。适用于 Opus 这类帧时长可选且能整除采样率的编码器。
       若 frameSamples 非 0，编码器优先使用 frameSamples；AAC-LC 忽略此字段。 */
    uint32_t frameDurationUs;
    /* 每个压缩包覆盖的准确每声道 sample 数。0 表示由 codec 根据 frameDurationUs 或自身
       固定规范决定；AAC-LC 固定为 1024。此字段避免把 1024/48000 错截断为 21ms。 */
    uint32_t frameSamples;
    /* 单个编码包允许的最大字节数；池和底层临时输出 buffer 据此预分配。 */
    size_t maxPacketBytes;
} AudioEncoderConfig;

typedef struct AudioEncoderOps {
    int (*pushPcm)(void *implementation, const AudioPcmFrame *frame);
    int (*flush)(void *implementation);
    /* 丢弃未凑满的一包 PCM，并复位 codec 的跨帧历史；不释放 encoder 句柄或 buffer。
       仅适用于 PCM 格式不变、但时间轴发生断裂的场景。 */
    int (*reset)(void *implementation);
    void (*close)(void *implementation);
} AudioEncoderOps;

typedef struct AudioEncoder {
    AudioCodec codec;
    void *implementation;
    const AudioEncoderOps *ops;
    AudioEncodedPacketCallback callback;
    void *callbackUserData;
} AudioEncoder;

void audio_encoder_config_init(AudioEncoderConfig *config);
void audio_encoder_set_packet_callback(AudioEncoder *encoder,
                                       AudioEncodedPacketCallback callback,
                                       void *userData);
int audio_encoder_init(AudioEncoder *encoder,
                       const AudioEncoderConfig *config,
                       const AudioPcmFormat *inputFormat);
int audio_encoder_push_pcm(AudioEncoder *encoder, const AudioPcmFrame *frame);
int audio_encoder_flush(AudioEncoder *encoder);
/* 轻量处理时间戳断裂。输入格式变化仍必须 close 后 init 新格式。 */
int audio_encoder_reset(AudioEncoder *encoder);
void audio_encoder_close(AudioEncoder *encoder);

#ifdef __cplusplus
}
#endif

#endif
