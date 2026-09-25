/*
 * 不依赖 ALSA 的编码器时间轴断裂自检。
 *
 * 每种 codec 都执行：
 *   断裂前半包 PCM -> audio_encoder_reset() -> 断裂后完整 PCM
 *
 * 通过条件：reset 不产生补零包；随后第一包使用断裂后的时间戳，证明没有把缺口两侧
 * 的 PCM 拼进同一个压缩包。
 */

#include "AudioCodec.h"
#include "AudioAac.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct ResetTestContext {
    uint64_t expectedTimestampUs;
    uint64_t actualTimestampUs;
    unsigned packetCount;
} ResetTestContext;

static int on_packet(const AudioEncodedPacket *packet, void *userData)
{
    ResetTestContext *context = (ResetTestContext *)userData;
    if (packet == NULL || context == NULL || packet->size == 0) {
        return -1;
    }
    ++context->packetCount;
    if (context->packetCount == 1) {
        context->actualTimestampUs = packet->timestampUs;
    }
    return 0;
}

static int run_case(AudioCodec codec, uint32_t frameSamples, uint32_t frameDurationUs)
{
    enum { kSampleRate = 48000, kChannels = 1, kBlockFrames = 480 };
    int16_t silence[kBlockFrames] = { 0 };
    const AudioPcmFormat format = {
        .sampleRate = kSampleRate,
        .channels = kChannels,
        .sampleFormat = AUDIO_SAMPLE_FORMAT_S16_LE,
    };
    const uint64_t beforeTimestampUs = 1000000;
    const uint64_t afterTimestampUs = 9000000;
    AudioEncoderConfig config;
    AudioEncoder encoder = { 0 };
    ResetTestContext context = { .expectedTimestampUs = afterTimestampUs };
    AudioPcmFrame frame = {
        .data = (const uint8_t *)silence,
        .frames = kBlockFrames,
        .format = format,
        .timestampUs = beforeTimestampUs,
    };
    unsigned blocksAfterReset;
    int result;

    audio_encoder_config_init(&config);
    config.codec = codec;
    config.bitrate = codec == AUDIO_CODEC_AAC ? 64000 : 32000;
    config.frameSamples = frameSamples;
    config.frameDurationUs = frameDurationUs;
    config.maxPacketBytes = 2048;
    audio_encoder_set_packet_callback(&encoder, on_packet, &context);
    if (audio_encoder_init(&encoder, &config, &format) != 0) {
        fprintf(stderr, "AudioEncoderResetDemo: init failed codec=%d\n", codec);
        return 1;
    }

    /* 喂半包，确保 reset 确实需要丢掉暂存 PCM。 */
    if (audio_encoder_push_pcm(&encoder, &frame) != 0 || context.packetCount != 0) {
        fprintf(stderr, "AudioEncoderResetDemo: unexpected pre-reset packet codec=%d\n", codec);
        audio_encoder_close(&encoder);
        return 1;
    }
    if (audio_encoder_reset(&encoder) != 0 || context.packetCount != 0) {
        fprintf(stderr, "AudioEncoderResetDemo: reset failed codec=%d\n", codec);
        audio_encoder_close(&encoder);
        return 1;
    }

    frame.timestampUs = afterTimestampUs;
    /* FDK-AAC 在 reset history 后允许有一帧 priming delay；连续送三组完整帧，
       既验证恢复继续出包，也不把 codec 固有 delay 误报成 reset 失败。 */
    blocksAfterReset = (frameSamples / kBlockFrames) * 3;
    for (unsigned index = 0; index < blocksAfterReset; ++index) {
        frame.timestampUs = afterTimestampUs + (uint64_t)index * 10000ULL;
        result = audio_encoder_push_pcm(&encoder, &frame);
        if (result != 0) {
            fprintf(stderr, "AudioEncoderResetDemo: push failed codec=%d error=%d\n", codec, result);
            audio_encoder_close(&encoder);
            return 1;
        }
    }

    const int passed = context.packetCount != 0
        && context.actualTimestampUs >= context.expectedTimestampUs;
    printf("AudioEncoderResetDemo: codec=%s packets=%u firstPts=%llu %s\n",
           codec == AUDIO_CODEC_AAC ? "AAC" : "Opus",
           context.packetCount,
           (unsigned long long)context.actualTimestampUs,
           passed ? "PASS" : "FAIL");
    audio_encoder_close(&encoder);
    return passed ? 0 : 1;
}

int main(void)
{
    return run_case(AUDIO_CODEC_AAC, AUDIO_AAC_LC_FRAME_SAMPLES, 0)
        || run_case(AUDIO_CODEC_OPUS, 960, 20000);
}
