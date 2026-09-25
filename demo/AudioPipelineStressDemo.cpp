#include "AudioPipeline.hpp"
#include "Log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>

namespace {

std::atomic<bool> g_stopRequested { false };

void onSignal(int)
{
    g_stopRequested.store(true);
}

bool waitFor(std::chrono::milliseconds duration)
{
    constexpr auto kStep = std::chrono::milliseconds(20);
    for (auto elapsed = std::chrono::milliseconds(0); elapsed < duration; elapsed += kStep) {
        if (g_stopRequested.load()) {
            return false;
        }
        std::this_thread::sleep_for(kStep);
    }
    return !g_stopRequested.load();
}

} // namespace

/*
 * 连续创建/销毁 APM->Opus 支路，同时维持一条 raw->Opus 常驻支路。
 *
 * 这验证 AudioSubscription 的 RAII 生命周期、weak Node 缓存清理、APM worker 的 join 和
 * raw 编码时间线互不干扰。它不测试 RTSP，也不测试真实 AEC 声学效果。
 */
int main(int argc, char** argv)
{
    const int cycles = argc >= 2 ? std::max(1, std::atoi(argv[1])) : 6;
    std::atomic<uint64_t> rawPackets { 0 };
    std::atomic<uint64_t> apmPackets { 0 };
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    AudioPipeline pipeline;
    if (!pipeline.startCapture()) {
        LOG_ERROR("AudioPipelineStress", "启动采集失败: " << pipeline.lastError());
        return 1;
    }

    AudioEncodedRequest rawRequest;
    const AudioSubscription rawSubscription = pipeline.subscribeEncoded(
        rawRequest,
        [&rawPackets](EncodedAudioPacketPtr packet) {
            if (packet) {
                ++rawPackets;
            }
        });
    if (!rawSubscription.valid()) {
        LOG_ERROR("AudioPipelineStress", "创建 raw Opus 支路失败: " << pipeline.lastError());
        pipeline.stopCapture();
        return 1;
    }

    bool passed = true;
    for (int cycle = 1; cycle <= cycles && !g_stopRequested.load(); ++cycle) {
        const uint64_t rawBefore = rawPackets.load();
        const uint64_t apmBefore = apmPackets.load();

        AudioEncodedRequest apmRequest;
        apmRequest.useApm = true;
        AudioSubscription apmSubscription = pipeline.subscribeEncoded(
            apmRequest,
            [&apmPackets](EncodedAudioPacketPtr packet) {
                if (packet) {
                    ++apmPackets;
                }
            });
        if (!apmSubscription.valid()) {
            LOG_ERROR("AudioPipelineStress", "cycle=" << cycle
                                                           << " 创建 APM 支路失败: " << pipeline.lastError());
            passed = false;
            break;
        }

        if (!waitFor(std::chrono::milliseconds(1200))) {
            break;
        }
        const uint64_t rawDuringApm = rawPackets.load();
        const uint64_t apmDuring = apmPackets.load();
        apmSubscription.reset();

        if (!waitFor(std::chrono::milliseconds(500))) {
            break;
        }
        const uint64_t rawAfter = rawPackets.load();
        const bool cyclePassed = rawDuringApm > rawBefore && rawAfter > rawDuringApm
            && apmDuring > apmBefore;
        LOG_INFO("AudioPipelineStress", "cycle=" << cycle
                                                      << (cyclePassed ? " PASS" : " FAIL")
                                                      << " raw=" << rawBefore << "->" << rawDuringApm
                                                      << "->" << rawAfter
                                                      << " apm=" << apmBefore << "->" << apmDuring);
        passed = passed && cyclePassed;
    }

    pipeline.stopCapture();
    passed = passed && !g_stopRequested.load() && rawPackets.load() > 10;
    LOG_INFO("AudioPipelineStress", (passed ? "PASS" : "FAIL")
                                         << " rawPackets=" << rawPackets.load()
                                         << " apmPackets=" << apmPackets.load());
    return passed ? 0 : 1;
}
