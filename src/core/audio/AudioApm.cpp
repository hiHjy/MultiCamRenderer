#include "AudioApm.h"

#include "api/audio/audio_processing.h"
#include "api/scoped_refptr.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

struct AudioApmImplementation {
    rtc::scoped_refptr<webrtc::AudioProcessing> processor;
    std::vector<int16_t> processedSamples;
    std::vector<int16_t> reverseProcessedSamples;
    AudioApmStatistics statistics {};
    bool bypass = false;
    bool echoCancellationEnabled = false;
    int aecStreamDelayMs = 0;
};

bool is_supported_format(const AudioPcmFormat &format) {
    return format.sampleFormat == AUDIO_SAMPLE_FORMAT_S16_LE &&
           format.channels >= 1 && format.channels <= 2 &&
           (format.sampleRate == 8000 || format.sampleRate == 16000 ||
            format.sampleRate == 32000 || format.sampleRate == 48000);
}

bool same_format(const AudioPcmFormat &left, const AudioPcmFormat &right) {
    return left.sampleRate == right.sampleRate &&
           left.channels == right.channels &&
           left.sampleFormat == right.sampleFormat;
}

uint64_t absolute_sample_sum(const int16_t *samples, size_t sampleCount) {
    uint64_t total = 0;

    for (size_t index = 0; index < sampleCount; ++index) {
        const int32_t sample = samples[index];
        total += static_cast<uint64_t>(sample < 0 ? -sample : sample);
    }
    return total;
}

/* AGC2 的参数范围（见上游 api/audio/audio_processing.h 的 GainController2）。 */
constexpr int kMaxGainDbLimit = 50;
constexpr int kHeadroomDbLimit = 31;
constexpr int kMaxGainChangeDbPerSecondLimit = 50;

int clamp_int(int value, int low, int high) {
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

}  // namespace

extern "C" void audio_apm_config_init(AudioApmConfig *config) {
    if (config == nullptr) {
        return;
    }
    std::memset(config, 0, sizeof(*config));
    /* 监控 RTSP 默认要的是完整环境声；APM 只在语音通话或明确配置时开启。 */
    config->enableAudioProcessing = 0;
    config->enableEchoCancellation = 0;
    config->aecStreamDelayMs = 0;
    /*
     * 默认关闭所有数字增益。
     *
     * 板端实测：AGC2 的自适应部分在信噪比本来就低的采集上会反复调整增益，
     * 听感是忽大忽小的"喘振"，比不加还难听。fixedDigitalGainDb 也为 0 时，
     * AGC2（包括 limiter）不会创建；默认路径只有降噪和高通。需要确定性补偿
     * 时由调用方设置固定增益；需要自动调节时再显式打开自适应增益。
     */
    config->enableAdaptiveDigitalGain = 0;
    config->maxGainDb = kMaxGainDbLimit;
    config->initialGainDb = 15;
    config->maxGainChangeDbPerSecond = 12;
    config->headroomDb = 5;
    config->maxOutputNoiseLevelDbfs = -50;
    /* 固定增益默认关闭：只有确认硬件侧存在固定衰减时才打开。 */
    config->fixedDigitalGainDb = 0;
    /*
     * 降噪和高通默认打开。后续若启用固定/自适应增益，底噪也会随之放大；这两级
     * 是让增益仍可用的前提。即使当前不加增益，它们也能改善底噪和低频隆隆声。
     */
    config->enableNoiseSuppression = 1;
    config->noiseSuppressionLevel = 2; /* High */
    config->enableHighPassFilter = 1;
    config->enableTransientSuppression = 0;
}

extern "C" int audio_apm_open(AudioApm *apm,
                              const AudioApmConfig *config,
                              AudioPcmFormat format,
                              size_t maximumFramesPerProcess) {
    AudioApmConfig effectiveConfig;
    auto implementation = std::make_unique<AudioApmImplementation>();
    webrtc::AudioProcessing::Config processingConfig;
    bool enableGainController2 = false;

    if (apm == nullptr || !is_supported_format(format) || maximumFramesPerProcess == 0 ||
        maximumFramesPerProcess % (format.sampleRate / 100) != 0) {
        return -EINVAL;
    }
    audio_apm_close(apm);
    audio_apm_config_init(&effectiveConfig);
    if (config != nullptr) {
        effectiveConfig = *config;
    }

    /*
     * 总开关关闭时，连 AudioProcessing 实例都不创建。process_capture() 仍保留相同
     * 的格式校验与输出接口，只把输入 frame 原样转交，方便上层无需写两条采集链。
     */
    if (!effectiveConfig.enableAudioProcessing) {
        implementation->bypass = true;
        apm->implementation = implementation.release();
        apm->format = format;
        apm->maximumFramesPerProcess = maximumFramesPerProcess;
        apm->initialized = 1;
        std::fprintf(stdout, "AudioApm: disabled, PCM bypass\n");
        return 0;
    }

    effectiveConfig.maxGainDb = clamp_int(effectiveConfig.maxGainDb, 0, kMaxGainDbLimit);
    effectiveConfig.initialGainDb = clamp_int(effectiveConfig.initialGainDb, 0, effectiveConfig.maxGainDb);
    effectiveConfig.headroomDb = clamp_int(effectiveConfig.headroomDb, 0, kHeadroomDbLimit);
    effectiveConfig.maxGainChangeDbPerSecond =
        clamp_int(effectiveConfig.maxGainChangeDbPerSecond, 1, kMaxGainChangeDbPerSecondLimit);
    effectiveConfig.maxOutputNoiseLevelDbfs = clamp_int(effectiveConfig.maxOutputNoiseLevelDbfs, -90, 0);
    effectiveConfig.fixedDigitalGainDb = clamp_int(effectiveConfig.fixedDigitalGainDb, 0, 90);
    effectiveConfig.aecStreamDelayMs = std::max(effectiveConfig.aecStreamDelayMs, 0);

    enableGainController2 = effectiveConfig.enableAdaptiveDigitalGain != 0 ||
                            effectiveConfig.fixedDigitalGainDb > 0;
    processingConfig.gain_controller2.enabled = enableGainController2;
    processingConfig.gain_controller2.adaptive_digital.enabled =
        effectiveConfig.enableAdaptiveDigitalGain != 0;
    processingConfig.gain_controller2.adaptive_digital.max_gain_db =
        static_cast<float>(effectiveConfig.maxGainDb);
    processingConfig.gain_controller2.adaptive_digital.initial_gain_db =
        static_cast<float>(effectiveConfig.initialGainDb);
    processingConfig.gain_controller2.adaptive_digital.max_gain_change_db_per_second =
        static_cast<float>(effectiveConfig.maxGainChangeDbPerSecond);
    processingConfig.gain_controller2.adaptive_digital.headroom_db =
        static_cast<float>(effectiveConfig.headroomDb);
    processingConfig.gain_controller2.adaptive_digital.max_output_noise_level_dbfs =
        static_cast<float>(effectiveConfig.maxOutputNoiseLevelDbfs);
    processingConfig.gain_controller2.fixed_digital.gain_db =
        static_cast<float>(effectiveConfig.fixedDigitalGainDb);

    /* mobile_mode=false 时 WebRTC AudioProcessingImpl 自动创建内建 AEC3，无需注入
       EchoControlFactory；factory 仅供替换为自定义回声消除器的高级用途。 */
    processingConfig.echo_canceller.enabled = effectiveConfig.enableEchoCancellation != 0;
    processingConfig.echo_canceller.mobile_mode = false;

    processingConfig.noise_suppression.enabled = effectiveConfig.enableNoiseSuppression != 0;
    switch (clamp_int(effectiveConfig.noiseSuppressionLevel, 0, 3)) {
    case 0:
        processingConfig.noise_suppression.level =
            webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
        break;
    case 1:
        processingConfig.noise_suppression.level =
            webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
        break;
    case 3:
        processingConfig.noise_suppression.level =
            webrtc::AudioProcessing::Config::NoiseSuppression::kVeryHigh;
        break;
    default:
        processingConfig.noise_suppression.level =
            webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
        break;
    }
    processingConfig.high_pass_filter.enabled = effectiveConfig.enableHighPassFilter != 0;
    processingConfig.transient_suppression.enabled = effectiveConfig.enableTransientSuppression != 0;

    implementation->processor = webrtc::AudioProcessingBuilder().SetConfig(processingConfig).Create();
    if (!implementation->processor) {
        return -ENOMEM;
    }

    implementation->processedSamples.resize(maximumFramesPerProcess * format.channels);
    implementation->reverseProcessedSamples.resize(maximumFramesPerProcess * format.channels);
    implementation->echoCancellationEnabled = effectiveConfig.enableEchoCancellation != 0;
    implementation->aecStreamDelayMs = effectiveConfig.aecStreamDelayMs;
    std::fprintf(stdout,
                 "AudioApm: AEC3=%s delay=%dms fixed=%ddB adaptive=%s NS=%s(level=%d) HPF=%s transient=%s\n",
                 implementation->echoCancellationEnabled ? "on" : "off",
                 implementation->aecStreamDelayMs,
                 effectiveConfig.fixedDigitalGainDb,
                 effectiveConfig.enableAdaptiveDigitalGain ? "on" : "off",
                 effectiveConfig.enableNoiseSuppression ? "on" : "off",
                 clamp_int(effectiveConfig.noiseSuppressionLevel, 0, 3),
                 effectiveConfig.enableHighPassFilter ? "on" : "off",
                 effectiveConfig.enableTransientSuppression ? "on" : "off");
    apm->implementation = implementation.release();
    apm->format = format;
    apm->maximumFramesPerProcess = maximumFramesPerProcess;
    apm->initialized = 1;
    return 0;
}

extern "C" int audio_apm_process_capture(AudioApm *apm,
                                           const AudioPcmFrame *input,
                                           AudioPcmFrame *output) {
    auto *implementation = apm == nullptr ? nullptr : static_cast<AudioApmImplementation *>(apm->implementation);
    const size_t framesPerTenMilliseconds = apm == nullptr ? 0 : apm->format.sampleRate / 100;
    const int16_t *inputSamples;
    int16_t *outputSamples;
    webrtc::StreamConfig streamConfig;

    if (implementation == nullptr || input == nullptr || output == nullptr || input->data == nullptr ||
        !same_format(input->format, apm->format) || framesPerTenMilliseconds == 0 ||
        input->frames == 0 || input->frames > apm->maximumFramesPerProcess ||
        input->frames % framesPerTenMilliseconds != 0) {
        return -EINVAL;
    }

    inputSamples = reinterpret_cast<const int16_t *>(input->data);
    if (implementation->bypass) {
        *output = *input;
        implementation->statistics.processedFrames += input->frames;
        implementation->statistics.processedSamples += input->frames * apm->format.channels;
        implementation->statistics.inputAbsoluteSampleSum += absolute_sample_sum(
            inputSamples, input->frames * apm->format.channels);
        implementation->statistics.outputAbsoluteSampleSum += absolute_sample_sum(
            inputSamples, input->frames * apm->format.channels);
        return 0;
    }

    outputSamples = implementation->processedSamples.data();
    streamConfig = webrtc::StreamConfig(static_cast<int>(apm->format.sampleRate),
                                        static_cast<size_t>(apm->format.channels));

    /* APM 固定按 10ms 一帧处理，调用方给的 period 可能更长（例如 20ms），这里拆开。 */
    for (size_t offset = 0; offset < input->frames; offset += framesPerTenMilliseconds) {
        const size_t sampleOffset = offset * apm->format.channels;
        if (implementation->echoCancellationEnabled &&
            implementation->processor->set_stream_delay_ms(implementation->aecStreamDelayMs) != 0) {
            return -EIO;
        }
        if (implementation->processor->ProcessStream(inputSamples + sampleOffset,
                                                     streamConfig,
                                                     streamConfig,
                                                     outputSamples + sampleOffset) != 0) {
            return -EIO;
        }
    }

    *output = *input;
    output->data = reinterpret_cast<const uint8_t *>(outputSamples);
    implementation->statistics.processedFrames += input->frames;
    implementation->statistics.processedSamples += input->frames * apm->format.channels;
    implementation->statistics.inputAbsoluteSampleSum += absolute_sample_sum(
        inputSamples, input->frames * apm->format.channels);
    implementation->statistics.outputAbsoluteSampleSum += absolute_sample_sum(
        outputSamples, input->frames * apm->format.channels);
    return 0;
}

extern "C" int audio_apm_process_reverse(AudioApm *apm, const AudioPcmFrame *frame) {
    auto *implementation = apm == nullptr ? nullptr : static_cast<AudioApmImplementation *>(apm->implementation);
    const size_t framesPerTenMilliseconds = apm == nullptr ? 0 : apm->format.sampleRate / 100;
    const int16_t *inputSamples;
    webrtc::StreamConfig streamConfig;

    if (implementation == nullptr || frame == nullptr || frame->data == nullptr ||
        !same_format(frame->format, apm->format) || framesPerTenMilliseconds == 0 ||
        frame->frames == 0 || frame->frames > apm->maximumFramesPerProcess ||
        frame->frames % framesPerTenMilliseconds != 0) {
        return -EINVAL;
    }
    if (implementation->bypass || !implementation->echoCancellationEnabled) {
        return -ENOTSUP;
    }

    inputSamples = reinterpret_cast<const int16_t *>(frame->data);
    streamConfig = webrtc::StreamConfig(static_cast<int>(apm->format.sampleRate),
                                        static_cast<size_t>(apm->format.channels));
    for (size_t offset = 0; offset < frame->frames; offset += framesPerTenMilliseconds) {
        const size_t sampleOffset = offset * apm->format.channels;
        if (implementation->processor->ProcessReverseStream(
                inputSamples + sampleOffset,
                streamConfig,
                streamConfig,
                implementation->reverseProcessedSamples.data() + sampleOffset) != 0) {
            return -EIO;
        }
    }
    return 0;
}

extern "C" void audio_apm_get_statistics(const AudioApm *apm,
                                           AudioApmStatistics *statistics) {
    const auto *implementation = apm == nullptr
                                     ? nullptr
                                     : static_cast<const AudioApmImplementation *>(apm->implementation);

    if (statistics == nullptr) {
        return;
    }
    std::memset(statistics, 0, sizeof(*statistics));
    if (implementation != nullptr) {
        *statistics = implementation->statistics;
    }
}

extern "C" void audio_apm_close(AudioApm *apm) {
    if (apm == nullptr) {
        return;
    }
    delete static_cast<AudioApmImplementation *>(apm->implementation);
    std::memset(apm, 0, sizeof(*apm));
}
