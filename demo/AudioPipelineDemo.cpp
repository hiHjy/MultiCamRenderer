#include "AudioPipeline.hpp"
#include "Log.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <thread>

namespace {

std::atomic<bool> g_stopRequested { false };

struct Statistics {
    std::atomic<uint64_t> rawPcmFrames { 0 };
    std::atomic<uint64_t> rawOpusPackets { 0 };
    std::atomic<uint64_t> apmOpusPackets { 0 };
};

void onSignal(int)
{
    g_stopRequested.store(true);
}

} // namespace

/*
 * 验证新的 C++ 音频图上行部分：
 *
 *   AudioCapture -> rawPcmHub -> raw Opus Encoder -> raw callback
 *                            -> APM -> Opus Encoder -> APM callback
 *
 * 运行到一半主动取消 APM 支路，之后 raw PCM/raw Opus 必须继续增长，以证明 APM Node 的
 * 创建、销毁不会重启或中断全局采集源和 raw 编码支路。
 */
int main(int argc, char** argv)
{
    const int runSeconds = argc >= 2 ? std::max(4, std::atoi(argv[1])) : 12;
    const bool enableApmBranch = argc < 3 || std::strcmp(argv[2], "raw-only") != 0;
    const int detachApmAfterSeconds = runSeconds / 2;
    Statistics statistics;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    AudioPipeline pipeline;
    if (!pipeline.startCapture()) {
        LOG_ERROR("AudioPipelineDemo", "启动采集失败: " << pipeline.lastError());
        return 1;
    }

    AudioPcmRequest rawPcmRequest;
    const AudioSubscription rawPcmSubscription = pipeline.subscribePcm(
        rawPcmRequest,
        [&statistics](AudioFramePtr frame) {
            if (frame) {
                ++statistics.rawPcmFrames;
            }
        });

    AudioEncodedRequest rawOpusRequest;
    const AudioSubscription rawOpusSubscription = pipeline.subscribeEncoded(
        rawOpusRequest,
        [&statistics](EncodedAudioPacketPtr packet) {
            if (packet) {
                ++statistics.rawOpusPackets;
            }
        });

    AudioSubscription apmOpusSubscription;
    if (enableApmBranch) {
        AudioEncodedRequest apmOpusRequest;
        apmOpusRequest.useApm = true;
        apmOpusSubscription = pipeline.subscribeEncoded(
            apmOpusRequest,
            [&statistics](EncodedAudioPacketPtr packet) {
                if (packet) {
                    ++statistics.apmOpusPackets;
                }
            });
    }

    if (!rawPcmSubscription.valid() || !rawOpusSubscription.valid()
        || (enableApmBranch && !apmOpusSubscription.valid())) {
        LOG_ERROR("AudioPipelineDemo", "创建订阅失败: " << pipeline.lastError());
        pipeline.stopCapture();
        return 1;
    }

    LOG_INFO("AudioPipelineDemo", "已启动 raw PCM / raw Opus"
                                      << (enableApmBranch ? " / APM Opus 三条支路" : " 两条支路")
                                      << (enableApmBranch ? "，中途取消 APM 支路" : ""));

    uint64_t rawPacketsBeforeApmDetach = 0;
    uint64_t apmPacketsBeforeDetach = 0;
    uint64_t rawPacketsAfterApmDetach = 0;
    bool apmDetached = false;
    for (int elapsedSeconds = 0; elapsedSeconds < runSeconds && !g_stopRequested.load(); ++elapsedSeconds) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        const uint64_t rawPcm = statistics.rawPcmFrames.load();
        const uint64_t rawPackets = statistics.rawOpusPackets.load();
        const uint64_t apmPackets = statistics.apmOpusPackets.load();
        LOG_INFO("AudioPipelineDemo", "t=" << elapsedSeconds + 1 << "s rawPcm=" << rawPcm
                                                << " rawOpus=" << rawPackets
                                                << " apmOpus=" << apmPackets);

        if (enableApmBranch && !apmDetached && elapsedSeconds + 1 >= detachApmAfterSeconds) {
            rawPacketsBeforeApmDetach = rawPackets;
            apmPacketsBeforeDetach = apmPackets;
            apmOpusSubscription.reset();
            apmDetached = true;
            LOG_INFO("AudioPipelineDemo", "已取消 APM Opus 订阅；raw 支路必须保持连续");
        } else if (apmDetached) {
            rawPacketsAfterApmDetach = rawPackets;
        }
    }

    pipeline.stopCapture();

    const bool rawPassed = statistics.rawPcmFrames.load() > 20 && statistics.rawOpusPackets.load() > 10;
    const bool apmPassed = !enableApmBranch || (rawPacketsBeforeApmDetach > 10
        && apmPacketsBeforeDetach > 5 && rawPacketsAfterApmDetach > rawPacketsBeforeApmDetach);
    const bool passed = rawPassed && apmPassed;
    LOG_INFO("AudioPipelineDemo", (passed ? "PASS" : "FAIL")
                                      << " rawPcm=" << statistics.rawPcmFrames.load()
                                      << " rawOpus=" << statistics.rawOpusPackets.load()
                                      << " apmOpus=" << statistics.apmOpusPackets.load());
    return passed ? 0 : 1;
}
