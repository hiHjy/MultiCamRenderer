#include "AudioFrame.hpp"
#include "AudioPacketFile.h"
#include "AudioPlaybackPipeline.hpp"
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

} // namespace

/*
 * 压缩音频播放验证：按录制时长读取 .mcropus 文件，送入正式 C++ 播放管线。
 *
 * 数据路径：AudioPacketFileReader -> EncodedAudioPacketPool -> push(Opus) -> playback worker
 *          -> AudioDecoder -> ALSA。
 * 用法：audio_playback_pipeline_opus_demo [file]，默认 audio_capture.opus。
 */
int main(int argc, char** argv)
{
    const char* const inputPath = argc >= 2 ? argv[1] : "audio_capture.opus";
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    AudioPacketFileReader reader {};
    const int openResult = audio_packet_file_reader_open(&reader, inputPath);
    if (openResult < 0) {
        LOG_ERROR("AudioPlaybackOpusDemo", "打开 " << inputPath << " 失败: " << openResult);
        return 1;
    }

    AudioPlaybackPipelineConfig config;
    config.playback.requestedFormat = reader.info.sourceFormat;
    config.playback.requestedPeriodFrames = reader.info.sourceFormat.sampleRate / 100;
    config.playback.requestedBufferFrames = config.playback.requestedPeriodFrames * 8;
    AudioPlaybackPipeline pipeline;
    if (!pipeline.start(config)) {
        LOG_ERROR("AudioPlaybackOpusDemo", "启动播放失败: " << pipeline.lastError());
        audio_packet_file_reader_close(&reader);
        return 1;
    }

    EncodedAudioPacketPool pool(16, 4096);
    uint64_t packetCount = 0;
    bool passed = true;
    auto nextPacketTime = std::chrono::steady_clock::now();
    LOG_INFO("AudioPlaybackOpusDemo", "开始播放 " << inputPath << " codec=" << reader.info.codec
                                                          << " source=" << reader.info.sourceFormat.sampleRate
                                                          << "Hz/" << reader.info.sourceFormat.channels << "ch");
    while (!g_stopRequested.load()) {
        AudioEncodedPacket source {};
        const int readResult = audio_packet_file_reader_read(&reader, &source);
        if (readResult == 0) {
            break;
        }
        if (readResult < 0) {
            LOG_ERROR("AudioPlaybackOpusDemo", "读取编码包失败: " << readResult);
            passed = false;
            break;
        }

        const EncodedAudioPacketPtr packet = pool.copyFrom(source);
        if (!packet || !pipeline.push(packet)) {
            LOG_ERROR("AudioPlaybackOpusDemo", "压缩包入队失败: " << pipeline.lastError());
            passed = false;
            break;
        }
        ++packetCount;
        /* 按媒体 duration 调度，但不累积 sleep_for 的普通线程唤醒误差。 */
        nextPacketTime += std::chrono::microseconds(std::max<uint32_t>(source.durationUs, 1000));
        std::this_thread::sleep_until(nextPacketTime);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const AudioPlaybackPipelineStatistics statistics = pipeline.statistics();
    pipeline.stop();
    audio_packet_file_reader_close(&reader);
    passed = passed && !g_stopRequested.load() && packetCount != 0 && statistics.decodeFailures == 0
        && statistics.playbackFailures == 0 && statistics.playedPcmFrames != 0;
    LOG_INFO("AudioPlaybackOpusDemo", (passed ? "PASS" : "FAIL")
                                            << " inputPackets=" << packetCount
                                            << " accepted=" << statistics.acceptedItems
                                            << " dropped=" << statistics.droppedQueuedItems
                                            << " decoded=" << statistics.decodedPackets
                                            << " playedFrames=" << statistics.playedPcmFrames);
    return passed ? 0 : 1;
}
