#include "AudioFrame.hpp"
#include "AudioPlaybackPipeline.hpp"
#include "Log.hpp"
#include "RtspStream.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <string>
#include <thread>

namespace {

std::atomic_bool g_stopRequested { false };

void onSignal(int)
{
    g_stopRequested.store(true);
}

} // namespace

/*
 * RTSP 音频接收闭环：
 *
 * RTSP AAC track -> Live555RtspClient -> RtspStream/Stream::onAudioPacket()
 * -> EncodedAudioPacketPool -> AudioPlaybackPipeline -> AAC decoder -> ALSA default。
 *
 * 视频仍由 RtspStream 的 DecodeWorker 正常接收；本 demo 在主线程持续取走裸帧，避免
 * readyQueue 满后干扰观察。用法：rtsp_audio_playback_demo [rtsp-url]。
 */
int main(int argc, char* argv[])
{
    const std::string url = argc >= 2 ? argv[1] : "rtsp://192.168.1.4:8554/main";

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    AudioPlaybackPipeline playback;
    AudioPlaybackPipelineConfig playbackConfig;
    // IPC 服务当前发布 AAC-LC 48kHz/mono；default ALSA 插件会按板级配置做必要声道
    // 转换。客户端将来支持动态重配时，再在 SDP 回调后按实际 audio format 启动播放。
    playbackConfig.playback.requestedFormat = {48000, 1, AUDIO_SAMPLE_FORMAT_S16_LE};
    playbackConfig.playback.requestedPeriodFrames = 480;
    playbackConfig.playback.requestedBufferFrames = 3840;
    if (!playback.start(playbackConfig)) {
        LOG_ERROR("RtspAudioPlaybackDemo", "启动 ALSA 播放失败: " << playback.lastError());
        return 1;
    }

    // 100ms 播放队列约保留 5 个 AAC access unit；16 个池对象覆盖收包与播放 worker
    // 短暂交接。池满时宁可丢当前实时包，也不阻塞 live555 RTP 收包线程。
    EncodedAudioPacketPool audioPool(16, 8192);
    std::atomic<uint64_t> receivedAudioPackets { 0 };
    std::atomic<uint64_t> audioPoolDrops { 0 };
    std::atomic<uint64_t> playbackRejectedPackets { 0 };

    RtspStream stream(url, 2);
    stream.setAudioPacketCallback([&](const AudioEncodedPacket& packet) {
        const EncodedAudioPacketPtr ownedPacket = audioPool.copyFrom(packet);
        if (!ownedPacket) {
            ++audioPoolDrops;
            return;
        }
        if (!playback.push(ownedPacket)) {
            ++playbackRejectedPackets;
            return;
        }
        ++receivedAudioPackets;
    });

    if (!stream.start()) {
        LOG_ERROR("RtspAudioPlaybackDemo", "启动 RTSP 拉流失败: " << stream.lastError());
        playback.stop();
        return 1;
    }

    LOG_INFO("RtspAudioPlaybackDemo", "已启动 RTSP AAC -> ALSA 播放 url=" << url
                                                                                << "，按 Ctrl+C 停止");
    auto lastStatisticsTime = std::chrono::steady_clock::now();
    uint64_t videoFrames = 0;
    while (!g_stopRequested.load()) {
        FramePacket frame;
        while (stream.tryGetFrame(frame)) {
            ++videoFrames;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastStatisticsTime >= std::chrono::seconds(2)) {
            const AudioPlaybackPipelineStatistics statistics = playback.statistics();
            const double elapsedSeconds = std::chrono::duration<double>(now - lastStatisticsTime).count();
            LOG_INFO("RtspAudioPlaybackDemo", "videoFps=" << videoFrames / elapsedSeconds
                                                               << " audioReceived=" << receivedAudioPackets.load()
                                                               << " poolDropped=" << audioPoolDrops.load()
                                                               << " pushRejected=" << playbackRejectedPackets.load()
                                                               << " decoded=" << statistics.decodedPackets
                                                               << " playedFrames=" << statistics.playedPcmFrames
                                                               << " playbackFailures=" << statistics.playbackFailures);
            videoFrames = 0;
            lastStatisticsTime = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    (void)stream.stop();
    const AudioPlaybackPipelineStatistics statistics = playback.statistics();
    playback.stop();
    const bool passed = receivedAudioPackets.load() != 0 && statistics.decodeFailures == 0
        && statistics.playbackFailures == 0;
    LOG_INFO("RtspAudioPlaybackDemo", (passed ? "PASS" : "FAIL")
                                            << " audioReceived=" << receivedAudioPackets.load()
                                            << " poolDropped=" << audioPoolDrops.load()
                                            << " pushRejected=" << playbackRejectedPackets.load()
                                            << " decoded=" << statistics.decodedPackets
                                            << " playedFrames=" << statistics.playedPcmFrames);
    return passed ? 0 : 1;
}
