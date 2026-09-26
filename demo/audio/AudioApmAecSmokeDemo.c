#include "AudioApm.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    kSampleRate = 48000,
    kChannels = 1,
    kFramesPerBlock = kSampleRate / 100,
    kBlockCount = 200,
};

static double mean_absolute_sample(const int16_t *samples, size_t sampleCount) {
    uint64_t total = 0;

    for (size_t index = 0; index < sampleCount; ++index) {
        const int32_t sample = samples[index];
        total += (uint64_t)(sample < 0 ? -sample : sample);
    }
    return sampleCount == 0 ? 0.0 : (double)total / (double)sampleCount;
}

/*
 * 不接真实扬声器/麦克风，只验证 WebRTC 内建 AEC3 的基础调用纪律：
 *
 * 10ms playback reference -> ProcessReverseStream()
 * 10ms synthetic capture  -> ProcessStream()
 *
 * 输出响度不用于评价消回声效果。真实效果依赖播放到麦克风的物理延迟与声学环境，
 * 应在未来双向通话链接通后单独实测。
 */
int main(void) {
    const AudioPcmFormat format = {
        .sampleRate = kSampleRate,
        .channels = kChannels,
        .sampleFormat = AUDIO_SAMPLE_FORMAT_S16_LE,
    };
    AudioApm apm;
    AudioApmConfig config;
    int16_t referenceSamples[kFramesPerBlock];
    int16_t captureSamples[kFramesPerBlock];
    double inputMean = 0.0;
    double outputMean = 0.0;

    memset(&apm, 0, sizeof(apm));
    audio_apm_config_init(&config);
    /* AEC smoke 必须显式开启总 APM；产品默认关闭是监控音频策略。 */
    config.enableAudioProcessing = 1;
    config.enableEchoCancellation = 1;
    config.aecStreamDelayMs = 0; /* smoke test 没有真实设备延迟，故只验证 API 路径。 */
    if (audio_apm_open(&apm, &config, format, kFramesPerBlock) < 0) {
        fprintf(stderr, "AudioApmAecSmokeDemo: AEC3 初始化失败\n");
        return 1;
    }

    for (int block = 0; block < kBlockCount; ++block) {
        AudioPcmFrame reference;
        AudioPcmFrame capture;
        AudioPcmFrame output;
        const uint64_t timestampUs = (uint64_t)block * 10000ULL;

        for (size_t sample = 0; sample < kFramesPerBlock; ++sample) {
            const double position = (double)(block * kFramesPerBlock + (int)sample) / kSampleRate;
            referenceSamples[sample] = (int16_t)(4000.0 * sin(2.0 * M_PI * 440.0 * position));
            /* 用 reference 加一点独立近端分量模拟 capture；不把它当真实声学测试。 */
            captureSamples[sample] = (int16_t)(referenceSamples[sample] +
                                                300.0 * sin(2.0 * M_PI * 700.0 * position));
        }

        reference.data = (const uint8_t *)referenceSamples;
        reference.frames = kFramesPerBlock;
        reference.format = format;
        reference.timestampUs = timestampUs;
        if (audio_apm_process_reverse(&apm, &reference) < 0) {
            fprintf(stderr, "AudioApmAecSmokeDemo: ProcessReverseStream 失败\n");
            audio_apm_close(&apm);
            return 1;
        }

        capture.data = (const uint8_t *)captureSamples;
        capture.frames = kFramesPerBlock;
        capture.format = format;
        capture.timestampUs = timestampUs;
        if (audio_apm_process_capture(&apm, &capture, &output) < 0) {
            fprintf(stderr, "AudioApmAecSmokeDemo: ProcessStream 失败\n");
            audio_apm_close(&apm);
            return 1;
        }
        inputMean += mean_absolute_sample(captureSamples, kFramesPerBlock);
        outputMean += mean_absolute_sample((const int16_t *)output.data, output.frames);
    }

    audio_apm_close(&apm);
    printf("AudioApmAecSmokeDemo: AEC3 reverse/capture 已完成 %d 个 10ms block, "
           "inputMean=%.1f outputMean=%.1f\n",
           kBlockCount,
           inputMean / kBlockCount,
           outputMean / kBlockCount);
    return 0;
}
