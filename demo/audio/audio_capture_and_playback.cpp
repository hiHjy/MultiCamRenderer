#include "AudioPipeline.hpp"
#include "AudioPlaybackPipeline.hpp"
#include "Log.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
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

void updatePeak(std::atomic<int>& peak, int value)
{
    int observed = peak.load(std::memory_order_relaxed);
    while (value > observed
           && !peak.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
    }
}

} // namespace

int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // 用 48 kHz、单声道、每块 480 frames（10ms）验证最常用的实时全双工配置。
    AudioPlaybackPipelineConfig playbackConfig;
    playbackConfig.playback.requestedFormat.channels = 1;
    playbackConfig.playback.requestedPeriodFrames = 480;
    playbackConfig.playback.requestedBufferFrames = 3840;
    AudioPlaybackPipeline playbackPipeline;
    if (!playbackPipeline.start(playbackConfig)) {
        LOG_ERROR("AudioCapturePlayback", "启动音频播放失败: " << playbackPipeline.lastError());
        return 1;
    }

    AudioPipelineConfig captureConfig;
    captureConfig.capture.requestedPeriodFrames = 480;
    captureConfig.capture.requestedChannels = 1;
    AudioPipeline pipeLine(captureConfig);
    if (!pipeLine.startCapture()) {
        LOG_ERROR("AudioCapturePlayback", "启动音频采集失败: " << pipeLine.lastError());
        playbackPipeline.stop();
        return 1;
    }

    // 1. 创建 PCM 订阅请求配置（可选择是否开启 APM 3A 处理）
    AudioPcmRequest request;
    // request.useApm = true; // 如需 3A 处理可设为 true

    // 2. 订阅 PCM 帧并实时推入播放管线
    std::atomic<uint64_t> capturedBlocks { 0 };
    std::atomic<uint64_t> rejectedBlocks { 0 };
    std::atomic<int> capturePeak { 0 };
    const AudioSubscription subscription = pipeLine.subscribePcm(
        request,
        [&playbackPipeline, &capturedBlocks, &rejectedBlocks, &capturePeak](AudioFramePtr frame) {
            if (!frame) {
                return;
            }

            /* 仅为这个直通诊断 demo 统计输入电平；正式 Hub/Node 热路径不做逐 sample 扫描。 */
            const AudioPcmFrame pcm = frame->pcmView();
            const int16_t* const samples = reinterpret_cast<const int16_t*>(pcm.data);
            const size_t sampleCount = pcm.frames * pcm.format.channels;
            int blockPeak = 0;
            for (size_t index = 0; index < sampleCount; ++index) {
                const int sample = samples[index];
                blockPeak = std::max(blockPeak, sample < 0 ? -sample : sample);
            }
            updatePeak(capturePeak, blockPeak);
            ++capturedBlocks;
            if (!playbackPipeline.push(std::move(frame))) {
                ++rejectedBlocks;
            }
        });

    if (!subscription.valid()) {
        LOG_ERROR("AudioCapturePlayback", "创建音频 PCM 订阅失败: " << pipeLine.lastError());
        pipeLine.stopCapture();
        playbackPipeline.stop();
        return 1;
    }

    LOG_INFO("AudioCapturePlayback", "音频采集与播放直通已启动，按 Ctrl+C 退出...");

    auto nextStatisticsLog = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!g_stopRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (std::chrono::steady_clock::now() >= nextStatisticsLog) {
            const AudioPlaybackPipelineStatistics statistics = playbackPipeline.statistics();
            LOG_INFO("AudioCapturePlayback", "capturedBlocks=" << capturedBlocks.load()
                                                                   << " capturePeak=" << capturePeak.load()
                                                                   << " pushRejected=" << rejectedBlocks.load()
                                                                   << " accepted=" << statistics.acceptedItems
                                                                   << " queueDropped=" << statistics.droppedQueuedItems
                                                                   << " playedFrames=" << statistics.playedPcmFrames
                                                                   << " playbackFailures=" << statistics.playbackFailures);
            nextStatisticsLog += std::chrono::seconds(1);
        }
    }

    LOG_INFO("AudioCapturePlayback", "收到退出信号，正在停止音频管线...");
    pipeLine.stopCapture();
    playbackPipeline.stop();
    LOG_INFO("AudioCapturePlayback", "音频采集与播放已停止，程序退出");

    return 0;
}
