#include "AudioApm.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    kSampleRate = 48000,
    kChannels = 1,
    kFramesPerBlock = kSampleRate / 100,
    kBlockCount = 6000,
    kStatisticsBlockCount = 100,
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
 * 用法: audio_apm_gain_demo [fixedGainDb] [maxGainDb] [initialGainDb] [adaptive(0|1)] [noiseFloorDbfs]
 *       不带参数时使用 audio_apm_config_init() 的默认配置。
 * 输入是固定的合成正弦，因此可以拿它当标尺，扫出配置到实际增益的映射。
 */
int main(int argc, char **argv) {
    const AudioPcmFormat format = {
        .sampleRate = kSampleRate,
        .channels = kChannels,
        .sampleFormat = AUDIO_SAMPLE_FORMAT_S16_LE,
    };
    AudioApm apm;
    AudioApmConfig config;
    int16_t inputSamples[kFramesPerBlock];
    double inputLevel = 0.0;
    double firstOutputLevel = 0.0;
    double finalOutputLevel = 0.0;

    memset(&apm, 0, sizeof(apm));
    audio_apm_config_init(&config);
    /* 本 demo 的目标是测 WebRTC 增益，不能继承监控 RTSP 的 APM 默认关闭策略。 */
    config.enableAudioProcessing = 1;
    if (argc >= 2) {
        config.fixedDigitalGainDb = atoi(argv[1]);
    }
    if (argc >= 3) {
        config.maxGainDb = atoi(argv[2]);
    }
    if (argc >= 4) {
        config.initialGainDb = atoi(argv[3]);
    }
    if (argc >= 5) {
        config.enableAdaptiveDigitalGain = atoi(argv[4]);
    }
    if (argc >= 6) {
        config.maxOutputNoiseLevelDbfs = atoi(argv[5]);
    }
    if (audio_apm_open(&apm, &config, format, kFramesPerBlock) < 0) {
        fprintf(stderr, "AudioApmGainDemo: 无法初始化 WebRTC APM\n");
        return 1;
    }

    /* 模拟持续且较小的单声道语音电平；总时长 60 秒，让自适应增益充分收敛。 */
    for (int block = 0; block < kBlockCount; ++block) {
        AudioPcmFrame input;
        AudioPcmFrame output;
        const uint64_t timestampUs = (uint64_t)block * 10000ULL;
        const double amplitude = 300.0 + (double)(block % 47) * 4.0;

        for (size_t sample = 0; sample < kFramesPerBlock; ++sample) {
            const double position = (double)(block * kFramesPerBlock + (int)sample) /
                                    (double)kSampleRate;
            inputSamples[sample] = (int16_t)(amplitude * sin(2.0 * M_PI * 440.0 * position));
        }
        input.data = (const uint8_t *)inputSamples;
        input.frames = kFramesPerBlock;
        input.format = format;
        input.timestampUs = timestampUs;
        if (audio_apm_process_capture(&apm, &input, &output) < 0) {
            fprintf(stderr, "AudioApmGainDemo: APM 处理失败\n");
            audio_apm_close(&apm);
            return 1;
        }

        if (block < kStatisticsBlockCount) {
            inputLevel += mean_absolute_sample(inputSamples, kFramesPerBlock);
            firstOutputLevel += mean_absolute_sample((const int16_t *)output.data, output.frames);
        }
        if (block >= kBlockCount - kStatisticsBlockCount) {
            finalOutputLevel += mean_absolute_sample((const int16_t *)output.data, output.frames);
        }
    }
    audio_apm_close(&apm);

    inputLevel /= kStatisticsBlockCount;
    firstOutputLevel /= kStatisticsBlockCount;
    finalOutputLevel /= kStatisticsBlockCount;
    printf("AudioApmGainDemo: inputMean=%.1f firstOutputMean=%.1f finalOutputMean=%.1f gain=%.2fx\n",
           inputLevel,
           firstOutputLevel,
           finalOutputLevel,
           inputLevel > 0.0 ? finalOutputLevel / inputLevel : 0.0);
    return 0;
}
