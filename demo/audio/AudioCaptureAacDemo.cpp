#include "AudioPacketFile.h"
#include "AudioAac.h"
#include "AudioPipeline.hpp"
#include "Log.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stopRequested { false };

void onSignal(int)
{
    g_stopRequested.store(true);
}

/*
 * 录制文件写盘必须离开 AudioHub 的采集/编码回调线程。这个小队列只属于 demo，生产 RTSP
 * 会换成自己的网络发送队列；满时丢旧包，保证不会因磁盘慢拖住 AudioPipeline。
 */
class AacPacketFileWriter {
public:
    explicit AacPacketFileWriter(std::string path)
        : m_path(std::move(path))
        , m_writerThread(&AacPacketFileWriter::writerMain, this)
    {
    }

    ~AacPacketFileWriter()
    {
        stop();
    }

    void enqueue(EncodedAudioPacketPtr packet)
    {
        if (!packet) {
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.size() == kQueueCapacity) {
            m_queue.pop_front();
            ++m_droppedPackets;
        }
        m_queue.push_back(std::move(packet));
        m_cv.notify_one();
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopRequested) {
                return;
            }
            m_stopRequested = true;
            m_cv.notify_all();
        }
        if (m_writerThread.joinable()) {
            m_writerThread.join();
        }
    }

    uint64_t writtenPackets() const { return m_writtenPackets.load(); }
    uint64_t droppedPackets() const { return m_droppedPackets.load(); }
    std::string error() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_error;
    }

private:
    void writerMain()
    {
        AudioPacketFileWriter writer {};
        bool opened = false;
        while (true) {
            EncodedAudioPacketPtr packet;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stopRequested || !m_queue.empty(); });
                if (m_queue.empty()) {
                    if (m_stopRequested) {
                        break;
                    }
                    continue;
                }
                packet = std::move(m_queue.front());
                m_queue.pop_front();
            }

            if (!opened) {
                AudioPacketFileInfo info {};
                info.codec = packet->codec;
                info.sourceFormat = packet->sourceFormat;
                info.frameSamples = packet->frameSamples;
                info.frameDurationUs = packet->durationUs;
                if (audio_packet_file_writer_open(&writer, m_path.c_str(), &info) < 0) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_error = "创建 AAC packet 文件失败";
                    continue;
                }
                opened = true;
            }

            const AudioEncodedPacket view = packet->packetView();
            if (audio_packet_file_writer_write(&writer, &view) < 0) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_error = "写入 AAC packet 文件失败";
                continue;
            }
            ++m_writtenPackets;
        }
        audio_packet_file_writer_close(&writer);
    }

    static constexpr size_t kQueueCapacity = 32;
    std::string m_path;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<EncodedAudioPacketPtr> m_queue;
    std::thread m_writerThread;
    std::atomic<uint64_t> m_writtenPackets { 0 };
    std::atomic<uint64_t> m_droppedPackets { 0 };
    bool m_stopRequested = false;
    std::string m_error;
};

} // namespace

/*
 * AudioPipeline AAC-LC 闭环的上游录制端。
 *
 * 数据路径：AudioCapture -> rawPcmHub -> EncoderNode(AAC-LC, 1024 samples) -> writer queue -> .mcraac。
 * 用法：audio_capture_aac_demo [output.mcraac] [seconds]，默认录制 5 秒。
 */
int main(int argc, char** argv)
{
    const std::string outputPath = argc >= 2 ? argv[1] : "audio_capture.mcraac";
    const int seconds = argc >= 3 ? std::max(1, std::atoi(argv[2])) : 5;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    AudioPipeline pipeline;
    AacPacketFileWriter writer(outputPath);
    AudioEncodedRequest request;
    request.encoderConfig.codec = AUDIO_CODEC_AAC;
    request.encoderConfig.bitrate = 64000;
    request.encoderConfig.frameDurationUs = 0; // AAC-LC 由规范固定为 1024 samples。
    request.encoderConfig.frameSamples = AUDIO_AAC_LC_FRAME_SAMPLES;
    request.encoderConfig.maxPacketBytes = 2048;

    const AudioSubscription subscription = pipeline.subscribeEncoded(
        request, [&writer](EncodedAudioPacketPtr packet) { writer.enqueue(std::move(packet)); });
    if (!subscription.valid() || !pipeline.startCapture()) {
        LOG_ERROR("AudioCaptureAacDemo", "创建 AAC 采集支路失败: " << pipeline.lastError());
        return 1;
    }

    LOG_INFO("AudioCaptureAacDemo", "正在采集 AAC-LC 48k PCM -> 1024-sample access unit，"
                                        << seconds << " 秒后停止，文件=" << outputPath);
    for (int elapsed = 0; elapsed < seconds && !g_stopRequested.load(); ++elapsed) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        LOG_INFO("AudioCaptureAacDemo", "t=" << elapsed + 1
                                                << "s packets=" << writer.writtenPackets()
                                                << " writerDropped=" << writer.droppedPackets());
    }

    pipeline.stopCapture();
    writer.stop();
    const bool passed = writer.writtenPackets() != 0 && writer.droppedPackets() == 0 && writer.error().empty();
    LOG_INFO("AudioCaptureAacDemo", (passed ? "PASS" : "FAIL")
                                        << " writtenPackets=" << writer.writtenPackets()
                                        << " dropped=" << writer.droppedPackets()
                                        << (writer.error().empty() ? "" : " error=" + writer.error()));
    return passed ? 0 : 1;
}
