#include "AudioFrame.hpp"
#include "AudioPlaybackPipeline.hpp"
#include "Log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stopRequested { false };

void onSignal(int)
{
    g_stopRequested.store(true);
}

} // namespace

/*
 * 读取 AudioPcmCaptureDemo 生成的原始 S16_LE PCM，按媒体节奏经正式播放管线输出。
 *
 * 用法：audio_pcm_playback_demo [input.pcm] [sampleRate] [channels] [framesPerBlock]
 * 默认：audio_capture.pcm 48000 1 480。
 */
int main(int argc, char** argv)
{
    const char* const inputPath = argc >= 2 ? argv[1] : "audio_capture.pcm";
    const uint32_t sampleRate = argc >= 3 ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10)) : 48000;
    const uint16_t channels = argc >= 4 ? static_cast<uint16_t>(std::strtoul(argv[3], nullptr, 10)) : 1;
    const size_t framesPerBlock = argc >= 5 ? std::max<size_t>(1, std::strtoul(argv[4], nullptr, 10)) : 480;
    if (sampleRate == 0 || channels == 0) {
        LOG_ERROR("AudioPcmPlaybackDemo", "sampleRate/channels 必须为正数");
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    FILE* const input = std::fopen(inputPath, "rb");
    if (input == nullptr) {
        LOG_ERROR("AudioPcmPlaybackDemo", "打开 " << inputPath << " 失败");
        return 1;
    }

    AudioPcmFormat format {};
    format.sampleRate = sampleRate;
    format.channels = channels;
    format.sampleFormat = AUDIO_SAMPLE_FORMAT_S16_LE;
    AudioPlaybackPipelineConfig config;
    config.playback.requestedFormat = format;
    config.playback.requestedPeriodFrames = sampleRate / 100;
    config.playback.requestedBufferFrames = config.playback.requestedPeriodFrames * 8;
    AudioPlaybackPipeline pipeline;
    if (!pipeline.start(config)) {
        LOG_ERROR("AudioPcmPlaybackDemo", "启动播放失败: " << pipeline.lastError());
        std::fclose(input);
        return 1;
    }

    const size_t bytesPerBlock = framesPerBlock * channels * sizeof(int16_t);
    AudioFramePool pool(16, bytesPerBlock);
    std::vector<uint8_t> bytes(bytesPerBlock);
    uint64_t pushedBlocks = 0;
    bool passed = true;
    auto nextBlockTime = std::chrono::steady_clock::now();
    LOG_INFO("AudioPcmPlaybackDemo", "开始播放 " << inputPath << " format=" << sampleRate
                                                       << "Hz/" << channels << "ch/S16_LE block="
                                                       << framesPerBlock << " frames");
    while (!g_stopRequested.load()) {
        const size_t readBytes = std::fread(bytes.data(), 1, bytes.size(), input);
        if (readBytes == 0) {
            break;
        }
        if (readBytes % (channels * sizeof(int16_t)) != 0) {
            LOG_ERROR("AudioPcmPlaybackDemo", "输入文件末尾不是完整 PCM frame");
            passed = false;
            break;
        }
        const AudioPcmFrame source { bytes.data(), readBytes / (channels * sizeof(int16_t)), format, 0 };
        const AudioFramePtr frame = pool.copyFrom(source);
        if (!frame || !pipeline.push(frame)) {
            LOG_ERROR("AudioPcmPlaybackDemo", "PCM 入队失败: " << pipeline.lastError());
            passed = false;
            break;
        }
        ++pushedBlocks;
        nextBlockTime += std::chrono::microseconds(source.frames * 1000000ULL / sampleRate);
        std::this_thread::sleep_until(nextBlockTime);
        if (readBytes != bytes.size()) {
            break;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const AudioPlaybackPipelineStatistics statistics = pipeline.statistics();
    pipeline.stop();
    std::fclose(input);
    passed = passed && !g_stopRequested.load() && pushedBlocks != 0 && statistics.playbackFailures == 0
        && statistics.playedPcmFrames != 0;
    LOG_INFO("AudioPcmPlaybackDemo", (passed ? "PASS" : "FAIL")
                                      << " pushedBlocks=" << pushedBlocks
                                      << " accepted=" << statistics.acceptedItems
                                      << " dropped=" << statistics.droppedQueuedItems
                                      << " playedFrames=" << statistics.playedPcmFrames
                                      << " playbackFailures=" << statistics.playbackFailures);
    return passed ? 0 : 1;
}
