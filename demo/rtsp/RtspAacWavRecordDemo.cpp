#include "AudioDecoder.h"
#include "AudioFrame.hpp"
#include "Live555RtspClient.hh"
#include "Log.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

std::atomic_bool g_stopRequested { false };

void onSignal(int)
{
    g_stopRequested.store(true);
}

void writeLe16(FILE* output, uint16_t value)
{
    const uint8_t bytes[] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF)};
    (void)std::fwrite(bytes, 1, sizeof(bytes), output);
}

void writeLe32(FILE* output, uint32_t value)
{
    const uint8_t bytes[] = {static_cast<uint8_t>(value & 0xFF),
                             static_cast<uint8_t>((value >> 8) & 0xFF),
                             static_cast<uint8_t>((value >> 16) & 0xFF),
                             static_cast<uint8_t>((value >> 24) & 0xFF)};
    (void)std::fwrite(bytes, 1, sizeof(bytes), output);
}

// 诊断文件只保存 S16_LE PCM；文件开始先留 44 字节 WAV header，结束后回填真实 data size。
bool writeWavHeader(FILE* output, const AudioPcmFormat& format, uint32_t pcmBytes)
{
    if (output == nullptr || format.sampleRate == 0 || format.channels == 0
        || format.sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE) {
        return false;
    }
    const uint16_t bytesPerSample = sizeof(int16_t);
    const uint16_t blockAlign = static_cast<uint16_t>(format.channels * bytesPerSample);
    const uint32_t byteRate = format.sampleRate * blockAlign;
    if (std::fseek(output, 0, SEEK_SET) != 0) {
        return false;
    }
    (void)std::fwrite("RIFF", 1, 4, output);
    writeLe32(output, 36U + pcmBytes);
    (void)std::fwrite("WAVEfmt ", 1, 8, output);
    writeLe32(output, 16);
    writeLe16(output, 1); // PCM
    writeLe16(output, format.channels);
    writeLe32(output, format.sampleRate);
    writeLe32(output, byteRate);
    writeLe16(output, blockAlign);
    writeLe16(output, bytesPerSample * 8);
    (void)std::fwrite("data", 1, 4, output);
    writeLe32(output, pcmBytes);
    return std::fseek(output, 0, SEEK_END) == 0;
}

// live555 client 输出的是没有 ADTS 头的 AAC raw_data_block。诊断时同时封装一份 ADTS，
// 便于用 ffplay/ffmpeg 独立验证“客户端收到的压缩 payload”本身。
bool writeAdtsAccessUnit(FILE* output, const EncodedAudioPacket& packet)
{
    if (output == nullptr || packet.codec != AUDIO_CODEC_AAC || packet.bytes.empty()
        || packet.sourceFormat.sampleRate != 48000 || packet.sourceFormat.channels != 1
        || packet.bytes.size() + 7 > 0x1FFF) {
        return false;
    }
    const unsigned fullSize = static_cast<unsigned>(packet.bytes.size() + 7);
    const uint8_t header[7] = {
        0xFF,
        0xF1, // MPEG-4, no CRC
        0x4C, // AAC-LC, 48kHz(index=3), mono 的高位
        static_cast<uint8_t>(0x40 | ((fullSize >> 11) & 0x03)),
        static_cast<uint8_t>((fullSize >> 3) & 0xFF),
        static_cast<uint8_t>(((fullSize & 0x07) << 5) | 0x1F),
        0xFC,
    };
    return std::fwrite(header, 1, sizeof(header), output) == sizeof(header)
        && std::fwrite(packet.bytes.data(), 1, packet.bytes.size(), output) == packet.bytes.size();
}

/*
 * 这个 demo 的收包线程只复制 AAC access unit 入固定队列。AAC 解码、WAV 文件 I/O 都在
 * writer 线程执行，因此磁盘慢或解码耗时不会阻塞 live555 RTP 事件循环。
 */
class AacWavRecorder {
public:
    explicit AacWavRecorder(std::string outputPath)
        : m_outputPath(std::move(outputPath))
        , m_packetPool(kQueueCapacity + 4, 8192)
    {
    }

    ~AacWavRecorder()
    {
        stop();
    }

    bool start()
    {
        m_output = std::fopen(m_outputPath.c_str(), "wb+");
        if (m_output == nullptr) {
            setError("创建 WAV 文件失败");
            return false;
        }
        // 占位 WAV header；收到第一帧 PCM 后才会知道实际 sampleRate/channels。
        const std::array<uint8_t, 44> emptyHeader {};
        if (std::fwrite(emptyHeader.data(), 1, emptyHeader.size(), m_output) != emptyHeader.size()) {
            setError("写入 WAV header 占位失败");
            std::fclose(m_output);
            m_output = nullptr;
            return false;
        }
        m_adtsOutput = std::fopen((m_outputPath + ".aac").c_str(), "wb");
        if (m_adtsOutput == nullptr) {
            setError("创建 AAC ADTS 诊断文件失败");
            std::fclose(m_output);
            m_output = nullptr;
            return false;
        }
        m_worker = std::thread(&AacWavRecorder::workerMain, this);
        return true;
    }

    // 运行在 live555 线程；只做有界复制和移动入队。
    void enqueue(const AudioEncodedPacket& packet)
    {
        const EncodedAudioPacketPtr copied = m_packetPool.copyFrom(packet);
        if (!copied) {
            ++m_poolDroppedPackets;
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queueCount == kQueueCapacity) {
                m_queue[m_queueHead].reset();
                m_queueHead = (m_queueHead + 1) % kQueueCapacity;
                --m_queueCount;
                ++m_queueDroppedPackets;
            }
            const size_t tail = (m_queueHead + m_queueCount) % kQueueCapacity;
            m_queue[tail] = copied;
            ++m_queueCount;
        }
        m_cv.notify_one();
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return;
            }
            m_stopping = true;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
        audio_decoder_close(&m_decoder);
        if (m_output != nullptr) {
            if (m_waveFormat.sampleRate != 0 && m_pcmBytes <= UINT32_MAX) {
                if (!writeWavHeader(m_output, m_waveFormat, static_cast<uint32_t>(m_pcmBytes))) {
                    setError("回填 WAV header 失败");
                }
            }
            std::fclose(m_output);
            m_output = nullptr;
        }
        if (m_adtsOutput != nullptr) {
            std::fclose(m_adtsOutput);
            m_adtsOutput = nullptr;
        }
    }

    uint64_t decodedPackets() const { return m_decodedPackets.load(); }
    uint64_t writtenPcmFrames() const { return m_writtenPcmFrames.load(); }
    uint64_t poolDroppedPackets() const { return m_poolDroppedPackets.load(); }
    uint64_t queueDroppedPackets() const { return m_queueDroppedPackets.load(); }
    std::string error() const
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_error;
    }

private:
    bool pop(EncodedAudioPacketPtr& packet)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_stopping || m_queueCount != 0; });
        if (m_queueCount == 0) {
            return false;
        }
        packet = std::move(m_queue[m_queueHead]);
        m_queueHead = (m_queueHead + 1) % kQueueCapacity;
        --m_queueCount;
        return true;
    }

    void workerMain()
    {
        EncodedAudioPacketPtr packet;
        while (pop(packet)) {
            if (!packet || packet->codec != AUDIO_CODEC_AAC) {
                continue;
            }
            if (!writeAdtsAccessUnit(m_adtsOutput, *packet)) {
                setError("写入 AAC ADTS 诊断文件失败");
                continue;
            }
            if (m_decoder.codec == AUDIO_CODEC_UNKNOWN) {
                audio_decoder_set_pcm_callback(&m_decoder, &AacWavRecorder::onDecodedPcm, this);
                if (audio_decoder_init(&m_decoder, AUDIO_CODEC_AAC, &packet->sourceFormat) < 0) {
                    setError("初始化 AAC 解码器失败");
                    continue;
                }
            }

            const AudioEncodedPacket view = packet->packetView();
            if (audio_decoder_decode_packet(&m_decoder, &view) < 0) {
                setError("AAC 解码失败");
                continue;
            }
            ++m_decodedPackets;
        }
    }

    static int onDecodedPcm(const AudioPcmFrame* frame, void* userData)
    {
        auto* const self = static_cast<AacWavRecorder*>(userData);
        if (self == nullptr || frame == nullptr || frame->data == nullptr || frame->frames == 0) {
            return -1;
        }
        if (self->m_waveFormat.sampleRate == 0) {
            self->m_waveFormat = frame->format;
        }
        if (self->m_waveFormat.sampleRate != frame->format.sampleRate
            || self->m_waveFormat.channels != frame->format.channels
            || self->m_waveFormat.sampleFormat != frame->format.sampleFormat) {
            self->setError("AAC 解码 PCM 格式在同一文件中发生变化");
            return -1;
        }
        const size_t bytes = frame->frames * frame->format.channels * sizeof(int16_t);
        if (std::fwrite(frame->data, 1, bytes, self->m_output) != bytes) {
            self->setError("写入 WAV PCM 失败");
            return -1;
        }
        self->m_pcmBytes += bytes;
        self->m_writtenPcmFrames += frame->frames;
        return 0;
    }

    void setError(const std::string& message)
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        if (m_error.empty()) {
            m_error = message;
        }
    }

    static constexpr size_t kQueueCapacity = 32;
    std::string m_outputPath;
    EncodedAudioPacketPool m_packetPool;
    std::array<EncodedAudioPacketPtr, kQueueCapacity> m_queue {};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    size_t m_queueHead = 0;
    size_t m_queueCount = 0;
    bool m_stopping = false;
    std::thread m_worker;
    AudioDecoder m_decoder {};
    FILE* m_output = nullptr;
    FILE* m_adtsOutput = nullptr;
    AudioPcmFormat m_waveFormat {};
    uint64_t m_pcmBytes = 0;
    std::atomic<uint64_t> m_decodedPackets { 0 };
    std::atomic<uint64_t> m_writtenPcmFrames { 0 };
    std::atomic<uint64_t> m_poolDroppedPackets { 0 };
    std::atomic<uint64_t> m_queueDroppedPackets { 0 };
    mutable std::mutex m_errorMutex;
    std::string m_error;
};

} // namespace

/*
 * 将 RTSP AAC-LC 音轨解码并写成标准 WAV，排查网络/AAC 与 ALSA 播放设备问题。
 * 用法：rtsp_aac_wav_record_demo [rtsp-url] [output.wav] [seconds]。
 */
int main(int argc, char* argv[])
{
    const std::string url = argc >= 2 ? argv[1] : "rtsp://192.168.1.4:8554/main";
    const std::string outputPath = argc >= 3 ? argv[2] : "/tmp/rtsp-aac.wav";
    const unsigned seconds = argc >= 4 ? static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10)) : 10;
    if (seconds == 0) {
        LOG_ERROR("RtspAacWavRecordDemo", "录制时长必须大于 0");
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    AacWavRecorder recorder(outputPath);
    if (!recorder.start()) {
        LOG_ERROR("RtspAacWavRecordDemo", recorder.error());
        return 1;
    }

    std::atomic_bool rtspError { false };
    Live555RtspClient client;
    if (!client.start(
            url,
            {},
            false,
            [&rtspError](Live555RtspClient::State state, const std::string& message) {
                if (state == Live555RtspClient::State::Error) {
                    LOG_ERROR("RtspAacWavRecordDemo", "RTSP 失败: " << message);
                    rtspError.store(true);
                }
            },
            [&recorder](const AudioEncodedPacket& packet) { recorder.enqueue(packet); })) {
        LOG_ERROR("RtspAacWavRecordDemo", "启动 RTSP 客户端失败: " << client.lastError());
        recorder.stop();
        return 1;
    }

    LOG_INFO("RtspAacWavRecordDemo", "录制 " << seconds << " 秒 RTSP AAC 到 " << outputPath);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!g_stopRequested.load() && !rtspError.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    client.stop();
    recorder.stop();

    const std::string recorderError = recorder.error();
    const bool passed = !rtspError.load() && recorderError.empty() && recorder.decodedPackets() != 0
        && recorder.writtenPcmFrames() != 0;
    LOG_INFO("RtspAacWavRecordDemo", (passed ? "PASS" : "FAIL")
                                           << " decodedPackets=" << recorder.decodedPackets()
                                           << " writtenFrames=" << recorder.writtenPcmFrames()
                                           << " poolDropped=" << recorder.poolDroppedPackets()
                                           << " queueDropped=" << recorder.queueDroppedPackets()
                                           << " file=" << outputPath
                                           << " adts=" << outputPath + ".aac"
                                           << (recorderError.empty() ? "" : " error=" + recorderError));
    return passed ? 0 : 1;
}
