#include "AudioFrame.hpp"
#include "AudioPlaybackPipeline.hpp"
#include "Log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <thread>

namespace {

std::atomic<bool> g_stopRequested { false };

void onSignal(int)
{
    g_stopRequested.store(true);
}

} // namespace

/*
 * PCM 直放验证：生成 440Hz 单声道 48kHz 音调，按 10ms 一帧送入正式 C++ 播放管线。
 *
 * 数据路径：AudioFramePool -> AudioPlaybackPipeline::push(PCM) -> playback worker -> ALSA。
 * 用法：audio_playback_pipeline_pcm_demo [seconds]，默认 3 秒。
 */
int main(int argc, char** argv)
{
    constexpr uint32_t kSampleRate = 48000;
    constexpr size_t kFramesPerBlock = kSampleRate / 100;
    constexpr size_t kBytesPerBlock = kFramesPerBlock * sizeof(int16_t);
    constexpr double kToneHz = 440.0;
    constexpr double kAmplitude = 6000.0;
    constexpr double kPi = 3.14159265358979323846;
    const int seconds = argc >= 2 ? std::max(1, std::atoi(argv[1])) : 3;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    AudioPlaybackPipeline pipeline;
    if (!pipeline.start()) {
        LOG_ERROR("AudioPlaybackPcmDemo", "启动播放失败: " << pipeline.lastError());
        return 1;
    }

    AudioPcmFormat format {};
    format.sampleRate = kSampleRate;
    format.channels = 1;
    format.sampleFormat = AUDIO_SAMPLE_FORMAT_S16_LE;
    AudioFramePool pool(16, kBytesPerBlock);
    std::array<uint8_t, kBytesPerBlock> bytes {};
    auto* samples = reinterpret_cast<int16_t*>(bytes.data());
    double phase = 0.0;
    const double phaseStep = 2.0 * kPi * kToneHz / kSampleRate;
    uint64_t timestampUs = 0;
    const int blocks = seconds * 100;
    bool passed = true;
    auto nextBlockTime = std::chrono::steady_clock::now();

    LOG_INFO("AudioPlaybackPcmDemo", "开始播放 " << seconds << " 秒 440Hz PCM 音调");
    for (int block = 0; block < blocks && !g_stopRequested.load(); ++block) {
        for (size_t index = 0; index < kFramesPerBlock; ++index) {
            samples[index] = static_cast<int16_t>(std::sin(phase) * kAmplitude);
            phase += phaseStep;
            if (phase >= 2.0 * kPi) {
                phase -= 2.0 * kPi;
            }
        }

        const AudioPcmFrame source { bytes.data(), kFramesPerBlock, format, timestampUs };
        const AudioFramePtr frame = pool.copyFrom(source);
        if (!frame || !pipeline.push(frame)) {
            LOG_ERROR("AudioPlaybackPcmDemo", "PCM 入队失败: " << pipeline.lastError());
            passed = false;
            break;
        }
        timestampUs += 10000;
        /* sleep_for 会把每次调度误差累积成声卡欠供；按绝对时间追赶采集时钟。 */
        nextBlockTime += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(nextBlockTime);
    }

    /* 给 worker 留一点时间播放末尾的硬件/软件队列。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const AudioPlaybackPipelineStatistics statistics = pipeline.statistics();
    pipeline.stop();
    passed = passed && !g_stopRequested.load() && statistics.playbackFailures == 0
        && statistics.playedPcmFrames >= kFramesPerBlock;
    LOG_INFO("AudioPlaybackPcmDemo", (passed ? "PASS" : "FAIL")
                                           << " accepted=" << statistics.acceptedItems
                                           << " dropped=" << statistics.droppedQueuedItems
                                           << " playedFrames=" << statistics.playedPcmFrames
                                           << " playbackFailures=" << statistics.playbackFailures);
    return passed ? 0 : 1;
}
