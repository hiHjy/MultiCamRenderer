#include "AudioPipeline.hpp"
#include "Log.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

std::atomic<bool> g_stopRequested { false };

void onSignal(int)
{
    g_stopRequested.store(true);
}

/* 文件写入必须脱离采集 callback；满时丢旧 PCM，保证 ALSA 采集不被磁盘反压。 */
class PcmFileQueue {
public:
    static constexpr size_t kCapacity = 128; // 48kHz 下 10ms x 128，约 1.28 秒。

    void push(AudioFramePtr frame)
    {
        if (!frame)
            return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_count == kCapacity) {
                m_slots[m_head].reset();
                m_head = (m_head + 1) % kCapacity;
                --m_count;
                ++m_droppedFrames;
            }
            const size_t tail = (m_head + m_count) % kCapacity;
            m_slots[tail] = std::move(frame);
            ++m_count;
        }
        m_cv.notify_one();
    }

    bool waitPop(AudioFramePtr& frame)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_stop || m_count != 0; });
        if (m_count == 0)
            return false;
        frame = std::move(m_slots[m_head]);
        m_head = (m_head + 1) % kCapacity;
        --m_count;
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
    }

    uint64_t droppedFrames() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_droppedFrames;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::array<AudioFramePtr, kCapacity> m_slots {};
    size_t m_head = 0;
    size_t m_count = 0;
    uint64_t m_droppedFrames = 0;
    bool m_stop = false;
};

} // namespace

/*
 * 只录原始 S16_LE PCM，不经过 Opus 或 APM。
 *
 * 用法：audio_pcm_capture_demo [output.pcm] [seconds]
 * 默认：audio_capture.pcm，5 秒。文件不含 WAV 头；播放时需提供日志中的 PCM 格式。
 */
int main(int argc, char** argv)
{
    const char* const outputPath = argc >= 2 ? argv[1] : "audio_capture.pcm";
    const int seconds = argc >= 3 ? std::max(1, std::atoi(argv[2])) : 5;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    AudioPipeline pipeline;
    if (!pipeline.startCapture()) {
        LOG_ERROR("AudioPcmCaptureDemo", "启动采集失败: " << pipeline.lastError());
        return 1;
    }
    const AudioPcmFormat format = pipeline.captureFormat();
    if (format.sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE) {
        LOG_ERROR("AudioPcmCaptureDemo", "当前 demo 只支持 S16_LE PCM");
        pipeline.stopCapture();
        return 1;
    }

    FILE* const output = std::fopen(outputPath, "wb");
    if (output == nullptr) {
        LOG_ERROR("AudioPcmCaptureDemo", "创建 " << outputPath << " 失败");
        pipeline.stopCapture();
        return 1;
    }

    PcmFileQueue fileQueue;
    std::atomic<uint64_t> writtenFrames { 0 };
    std::atomic<int> writeFailed { 0 };
    std::thread writer([&] {
        AudioFramePtr frame;
        while (fileQueue.waitPop(frame)) {
            const AudioPcmFrame pcm = frame->pcmView();
            const size_t bytes = pcm.frames * pcm.format.channels * sizeof(int16_t);
            if (std::fwrite(pcm.data, 1, bytes, output) != bytes) {
                writeFailed.store(1);
                LOG_ERROR("AudioPcmCaptureDemo", "写入 raw PCM 失败");
                continue;
            }
            writtenFrames += pcm.frames;
        }
    });

    const AudioPcmRequest request;
    AudioSubscription subscription = pipeline.subscribePcm(
        request, [&fileQueue](AudioFramePtr frame) { fileQueue.push(std::move(frame)); });
    if (!subscription.valid()) {
        LOG_ERROR("AudioPcmCaptureDemo", "订阅 raw PCM 失败: " << pipeline.lastError());
        fileQueue.stop();
        writer.join();
        std::fclose(output);
        pipeline.stopCapture();
        return 1;
    }

    LOG_INFO("AudioPcmCaptureDemo", "开始录制 " << seconds << " 秒 raw PCM 到 " << outputPath
                                                      << "，format=" << format.sampleRate << "Hz/"
                                                      << format.channels << "ch/S16_LE");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!g_stopRequested.load() && !writeFailed.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    subscription.reset();
    pipeline.stopCapture();
    fileQueue.stop();
    writer.join();
    std::fclose(output);

    const bool passed = !writeFailed.load() && writtenFrames.load() != 0;
    LOG_INFO("AudioPcmCaptureDemo", (passed ? "PASS" : "FAIL")
                                     << " writtenFrames=" << writtenFrames.load()
                                     << " queueDropped=" << fileQueue.droppedFrames()
                                     << " file=" << outputPath);
    return passed ? 0 : 1;
}
