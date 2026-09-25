#include "AudioDiagnosticCapture.hpp"

#include "AudioPacketFile.h"
#include "Log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

namespace {

struct EncodedRecord {
    AudioEncodedPacket metadata {};
    std::vector<uint8_t> bytes;
};

struct Snapshot {
    AudioPcmFormat pcmFormat {};
    AudioEncodedStreamInfo streamInfo {};
    uint64_t firstRawTimestampUs = 0;
    uint64_t endRawTimestampUs = 0;
    uint64_t rawBlocks = 0;
    uint64_t timestampDiscontinuities = 0;
    std::vector<uint8_t> rawBytes;
    std::vector<EncodedRecord> encodedPackets;
};

uint64_t pcmDurationUs(size_t frames, const AudioPcmFormat& format)
{
    return format.sampleRate == 0 ? 0 : frames * 1000000ULL / format.sampleRate;
}

size_t pcmBytes(const AudioFrame& frame)
{
    return frame.frames * frame.format.channels * sizeof(int16_t);
}

void writeLe16(FILE* file, uint16_t value)
{
    const uint8_t bytes[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8U)};
    (void)std::fwrite(bytes, 1, sizeof(bytes), file);
}

void writeLe32(FILE* file, uint32_t value)
{
    const uint8_t bytes[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8U),
                              static_cast<uint8_t>(value >> 16U), static_cast<uint8_t>(value >> 24U)};
    (void)std::fwrite(bytes, 1, sizeof(bytes), file);
}

bool writeWavFile(const std::string& path, const Snapshot& snapshot)
{
    if (snapshot.pcmFormat.sampleRate == 0 || snapshot.pcmFormat.channels == 0
        || snapshot.pcmFormat.sampleFormat != AUDIO_SAMPLE_FORMAT_S16_LE
        || snapshot.rawBytes.size() > std::numeric_limits<uint32_t>::max() - 36U) {
        return false;
    }

    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }

    const uint32_t dataBytes = static_cast<uint32_t>(snapshot.rawBytes.size());
    const uint32_t bytesPerSecond = snapshot.pcmFormat.sampleRate * snapshot.pcmFormat.channels * 2U;
    const uint16_t blockAlign = static_cast<uint16_t>(snapshot.pcmFormat.channels * 2U);
    bool ok = std::fwrite("RIFF", 1, 4, file) == 4;
    writeLe32(file, 36U + dataBytes);
    ok = ok && std::fwrite("WAVEfmt ", 1, 8, file) == 8;
    writeLe32(file, 16U);
    writeLe16(file, 1U); // PCM
    writeLe16(file, snapshot.pcmFormat.channels);
    writeLe32(file, snapshot.pcmFormat.sampleRate);
    writeLe32(file, bytesPerSecond);
    writeLe16(file, blockAlign);
    writeLe16(file, 16U);
    ok = ok && std::fwrite("data", 1, 4, file) == 4;
    writeLe32(file, dataBytes);
    ok = ok && (dataBytes == 0 || std::fwrite(snapshot.rawBytes.data(), 1, dataBytes, file) == dataBytes);
    ok = ok && std::fclose(file) == 0;
    return ok;
}

int aacSampleRateIndex(uint32_t sampleRate)
{
    constexpr uint32_t kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                   22050, 16000, 12000, 11025, 8000,  7350};
    for (size_t index = 0; index < sizeof(kRates) / sizeof(kRates[0]); ++index) {
        if (kRates[index] == sampleRate) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool writeAdtsHeader(FILE* file, size_t payloadBytes, const AudioPcmFormat& format)
{
    const int sampleRateIndex = aacSampleRateIndex(format.sampleRate);
    const size_t totalBytes = payloadBytes + 7U;
    if (sampleRateIndex < 0 || format.channels == 0 || format.channels > 7
        || totalBytes > 0x1fffU) {
        return false;
    }

    const uint8_t profile = 1; // MPEG-4 AAC-LC object type 2, ADTS 写 objectType - 1。
    const uint8_t channels = static_cast<uint8_t>(format.channels);
    const uint16_t frameLength = static_cast<uint16_t>(totalBytes);
    const uint8_t header[7] = {
        0xff,
        0xf1, // MPEG-4, 无 CRC
        static_cast<uint8_t>((profile << 6U) | (sampleRateIndex << 2U) | (channels >> 2U)),
        static_cast<uint8_t>(((channels & 0x03U) << 6U) | (frameLength >> 11U)),
        static_cast<uint8_t>(frameLength >> 3U),
        static_cast<uint8_t>(((frameLength & 0x07U) << 5U) | 0x1fU),
        0xfc,
    };
    return std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
}

bool writeEncodedFiles(const std::string& packetPath,
                       const std::string& adtsPath,
                       const Snapshot& snapshot,
                       size_t& writtenPackets)
{
    if (snapshot.streamInfo.codec != AUDIO_CODEC_AAC || snapshot.streamInfo.sourceFormat.sampleRate == 0) {
        return false;
    }

    AudioPacketFileWriter packetWriter {};
    const AudioPacketFileInfo info {
        snapshot.streamInfo.codec,
        snapshot.streamInfo.sourceFormat,
        snapshot.streamInfo.frameSamples,
        snapshot.streamInfo.frameSamples == 0
            ? 0
            : static_cast<uint32_t>(static_cast<uint64_t>(snapshot.streamInfo.frameSamples) * 1000000ULL
                                    / snapshot.streamInfo.sourceFormat.sampleRate),
    };
    if (audio_packet_file_writer_open(&packetWriter, packetPath.c_str(), &info) != 0) {
        return false;
    }

    FILE* adtsFile = std::fopen(adtsPath.c_str(), "wb");
    if (adtsFile == nullptr) {
        audio_packet_file_writer_close(&packetWriter);
        return false;
    }

    bool ok = true;
    writtenPackets = 0;
    for (const EncodedRecord& record : snapshot.encodedPackets) {
        AudioEncodedPacket packet = record.metadata;
        packet.data = record.bytes.data();
        packet.size = record.bytes.size();
        if (packet.size == 0 || audio_packet_file_writer_write(&packetWriter, &packet) != 0
            || !writeAdtsHeader(adtsFile, packet.size, packet.sourceFormat)
            || std::fwrite(packet.data, 1, packet.size, adtsFile) != packet.size) {
            ok = false;
            break;
        }
        ++writtenPackets;
    }
    audio_packet_file_writer_close(&packetWriter);
    ok = std::fclose(adtsFile) == 0 && ok;
    return ok;
}

bool writeMetadataFile(const std::string& path, const Snapshot& snapshot, size_t encodedPackets)
{
    FILE* file = std::fopen(path.c_str(), "w");
    if (file == nullptr) {
        return false;
    }
    const uint64_t durationUs = snapshot.endRawTimestampUs > snapshot.firstRawTimestampUs
        ? snapshot.endRawTimestampUs - snapshot.firstRawTimestampUs
        : 0;
    const int result = std::fprintf(file,
                                    "raw_pcm=%uHz/%uch S16_LE\n"
                                    "raw_blocks=%llu\n"
                                    "raw_bytes=%zu\n"
                                    "raw_start_pts_us=%llu\n"
                                    "raw_duration_us=%llu\n"
                                    "raw_timestamp_discontinuities=%llu\n"
                                    "encoded_codec=AAC-LC\n"
                                    "encoded_packets=%zu\n"
                                    "encoded_frame_samples=%u\n",
                                    snapshot.pcmFormat.sampleRate,
                                    snapshot.pcmFormat.channels,
                                    static_cast<unsigned long long>(snapshot.rawBlocks),
                                    snapshot.rawBytes.size(),
                                    static_cast<unsigned long long>(snapshot.firstRawTimestampUs),
                                    static_cast<unsigned long long>(durationUs),
                                    static_cast<unsigned long long>(snapshot.timestampDiscontinuities),
                                    encodedPackets,
                                    snapshot.streamInfo.frameSamples);
    return result >= 0 && std::fclose(file) == 0;
}

bool writeReadmeFile(const std::string& directory)
{
    const std::filesystem::path path = std::filesystem::path(directory) / "README_AUDIO_DIAGNOSTIC.txt";
    FILE* file = std::fopen(path.c_str(), "w");
    if (file == nullptr) {
        return false;
    }
    const char text[] =
        "MultiCamRenderer 音频故障快照说明\n"
        "\n"
        "触发一次新的 30 秒取证：\n"
        "  /root/audio-diagnostics/capture_audio_snapshot.sh\n"
        "\n"
        "脚本的实际动作等价于：\n"
        "  kill -USR1 $(ps | grep '[m]ulticam_ipc_app' | awk '{print $1}')\n"
        "\n"
        "每次成功取证会生成同一时间段、同一文件名前缀的四个文件：\n"
        "  *-raw.wav             ALSA 采集后、APM/编码前的原始 PCM。\n"
        "  *-encoded.aac         AAC 编码后的 ADTS 文件，可用 ffplay 直接播放。\n"
        "  *-encoded.mcraudio    保留每个 AAC access unit PTS 的项目诊断文件。\n"
        "  *.txt                 PCM 格式、时长、PTS 间断次数、编码包数量。\n"
        "\n"
        "故障时不要先重启：先执行上面的 kill -USR1，等待约 30 秒看到快照完成日志，\n"
        "再分别播放 raw.wav 和 encoded.aac。raw.wav 已异常说明问题在采集/ALSA 一侧；\n"
        "raw.wav 正常而 encoded.aac 异常，说明问题在编码或之后的链路。\n";
    const bool ok = std::fwrite(text, 1, sizeof(text) - 1U, file) == sizeof(text) - 1U;
    return ok && std::fclose(file) == 0;
}

} // namespace

struct AudioDiagnosticCapture::Session {
    explicit Session(const AudioPcmFormat& inputFormat, const AudioEncodedStreamInfo& inputStreamInfo)
        : pcmFormat(inputFormat)
        , streamInfo(inputStreamInfo)
    {
        // RV1126B 默认 48kHz/mono/10ms：30 秒约 2.88 MiB；只在一次诊断期间分配。
        rawBytes.reserve(static_cast<size_t>(pcmFormat.sampleRate) * pcmFormat.channels * sizeof(int16_t) * 31U);
        encodedPackets.reserve(1500);
    }

    std::mutex mutex;
    bool accepting = true;
    AudioPcmFormat pcmFormat {};
    AudioEncodedStreamInfo streamInfo {};
    uint64_t firstRawTimestampUs = 0;
    uint64_t endRawTimestampUs = 0;
    uint64_t rawBlocks = 0;
    uint64_t timestampDiscontinuities = 0;
    std::vector<uint8_t> rawBytes;
    std::vector<EncodedRecord> encodedPackets;
};

AudioDiagnosticCapture::AudioDiagnosticCapture(const AudioDiagnosticCaptureConfig& config)
    : m_config(config)
{
    m_config.durationSeconds = std::max<uint32_t>(1, m_config.durationSeconds);
}

AudioDiagnosticCapture::~AudioDiagnosticCapture()
{
    cancel();
}

bool AudioDiagnosticCapture::startOneShot(AudioPipeline& pipeline)
{
    /* 上一轮 monitor 已结束时 thread 仍是 joinable，下一轮创建前必须回收它。 */
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_capturing) {
            setErrorLocked("已有音频取证任务正在进行");
            return false;
        }
    }
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }

    const AudioPcmFormat pcmFormat = pipeline.captureFormat();
    AudioEncodedStreamInfo streamInfo {};
    if (pcmFormat.sampleRate == 0 || pcmFormat.channels == 0
        || !pipeline.getEncodedStreamInfo(m_config.encodedCodec, m_config.bitratePreset, streamInfo)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        setErrorLocked("无法开始音频取证: " + pipeline.lastError());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError.clear();
        m_cancelRequested = false;
        m_capturing = true;
    }

    const auto session = std::make_shared<Session>(pcmFormat, streamInfo);
    const std::weak_ptr<Session> weakSession = session;
    AudioPcmRequest rawRequest {};
    m_pcmSubscription = pipeline.subscribePcm(rawRequest, [weakSession](AudioFramePtr frame) {
        recordPcm(weakSession, std::move(frame));
    });
    m_encodedSubscription = pipeline.subscribeEncoded(m_config.encodedCodec, m_config.bitratePreset,
                                                       [weakSession](EncodedAudioPacketPtr packet) {
                                                           recordEncoded(weakSession, std::move(packet));
                                                       });
    if (!m_pcmSubscription.valid() || !m_encodedSubscription.valid()) {
        m_pcmSubscription.reset();
        m_encodedSubscription.reset();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_capturing = false;
        setErrorLocked("注册音频取证订阅失败: " + pipeline.lastError());
        return false;
    }

    m_monitorThread = std::thread(&AudioDiagnosticCapture::monitorMain, this, session);
    LOG_INFO("AudioDiagnosticCapture", "已开始一次性音频取证 duration=" << m_config.durationSeconds
                                                                                << "s，结束后写入 "
                                                                                << m_config.outputDirectory);
    return true;
}

void AudioDiagnosticCapture::cancel()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_capturing && !m_monitorThread.joinable()) {
            return;
        }
        m_cancelRequested = true;
    }
    m_monitorCv.notify_one();
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }
}

bool AudioDiagnosticCapture::isCapturing() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_capturing;
}

std::string AudioDiagnosticCapture::lastError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

void AudioDiagnosticCapture::recordPcm(const std::weak_ptr<Session>& weakSession, AudioFramePtr frame)
{
    const std::shared_ptr<Session> session = weakSession.lock();
    if (!session || !frame) {
        return;
    }

    const size_t bytes = pcmBytes(*frame);
    if (bytes == 0 || bytes > frame->samples.size()) {
        return;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!session->accepting || frame->format.sampleRate != session->pcmFormat.sampleRate
        || frame->format.channels != session->pcmFormat.channels
        || frame->format.sampleFormat != session->pcmFormat.sampleFormat) {
        return;
    }
    if (session->rawBlocks == 0) {
        session->firstRawTimestampUs = frame->timestampUs;
    } else if (frame->timestampUs != session->endRawTimestampUs) {
        ++session->timestampDiscontinuities;
    }
    session->rawBytes.insert(session->rawBytes.end(), frame->samples.begin(), frame->samples.begin() + bytes);
    ++session->rawBlocks;
    session->endRawTimestampUs = frame->timestampUs + pcmDurationUs(frame->frames, frame->format);
}

void AudioDiagnosticCapture::recordEncoded(const std::weak_ptr<Session>& weakSession,
                                           EncodedAudioPacketPtr packet)
{
    const std::shared_ptr<Session> session = weakSession.lock();
    if (!session || !packet || packet->codec != session->streamInfo.codec || packet->bytes.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(session->mutex);
    if (!session->accepting) {
        return;
    }
    EncodedRecord record;
    record.metadata = packet->packetView();
    record.bytes = packet->bytes;
    record.metadata.data = nullptr; // record.bytes owns data; 写文件时再补回正确地址。
    record.metadata.size = record.bytes.size();
    session->encodedPackets.push_back(std::move(record));
}

void AudioDiagnosticCapture::monitorMain(std::shared_ptr<Session> session)
{
    bool cancelled = false;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        cancelled = m_monitorCv.wait_for(lock, std::chrono::seconds(m_config.durationSeconds), [this] {
            return m_cancelRequested;
        });
    }
    finishSession(session, !cancelled);
}

void AudioDiagnosticCapture::finishSession(const std::shared_ptr<Session>& session, bool writeFiles)
{
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        session->accepting = false;
    }
    m_pcmSubscription.reset();
    m_encodedSubscription.reset();

    Snapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        snapshot.pcmFormat = session->pcmFormat;
        snapshot.streamInfo = session->streamInfo;
        snapshot.firstRawTimestampUs = session->firstRawTimestampUs;
        snapshot.endRawTimestampUs = session->endRawTimestampUs;
        snapshot.rawBlocks = session->rawBlocks;
        snapshot.timestampDiscontinuities = session->timestampDiscontinuities;
        snapshot.rawBytes = std::move(session->rawBytes);
        snapshot.encodedPackets = std::move(session->encodedPackets);
    }

    if (writeFiles) {
        /* 编码器存在一个很短的异步队列；只保留与 raw 快照同一时间线的 AAC access unit。 */
        snapshot.encodedPackets.erase(
            std::remove_if(snapshot.encodedPackets.begin(), snapshot.encodedPackets.end(), [&snapshot](const EncodedRecord& record) {
                return snapshot.firstRawTimestampUs == 0 || record.metadata.timestampUs < snapshot.firstRawTimestampUs
                    || record.metadata.timestampUs >= snapshot.endRawTimestampUs;
            }),
            snapshot.encodedPackets.end());

        const uint64_t epochMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        const std::filesystem::path directory(m_config.outputDirectory);
        const std::string base = (directory / ("mcr-audio-snapshot-" + std::to_string(epochMs))).string();
        std::error_code directoryError;
        std::filesystem::create_directories(directory, directoryError);
        const std::string wavPath = base + "-raw.wav";
        const std::string packetPath = base + "-encoded.mcraudio";
        const std::string adtsPath = base + "-encoded.aac";
        const std::string metadataPath = base + ".txt";
        size_t writtenPackets = 0;
        const bool readmeOk = !directoryError && writeReadmeFile(m_config.outputDirectory);
        const bool rawOk = !directoryError && writeWavFile(wavPath, snapshot);
        const bool encodedOk = !directoryError
            && writeEncodedFiles(packetPath, adtsPath, snapshot, writtenPackets);
        const bool metadataOk = !directoryError && writeMetadataFile(metadataPath, snapshot, writtenPackets);
        if (readmeOk && rawOk && encodedOk && metadataOk) {
            LOG_INFO("AudioDiagnosticCapture", "音频快照完成 raw=" << wavPath
                                                                          << " encoded=" << packetPath
                                                                          << " adts=" << adtsPath
                                                                          << " blocks=" << snapshot.rawBlocks
                                                                          << " packets=" << writtenPackets
                                                                          << " timestampGaps="
                                                                          << snapshot.timestampDiscontinuities);
        } else {
            std::lock_guard<std::mutex> lock(m_mutex);
            setErrorLocked("音频快照写文件失败，目录=" + m_config.outputDirectory);
            LOG_ERROR("AudioDiagnosticCapture", m_lastError);
        }
    } else {
        LOG_INFO("AudioDiagnosticCapture", "音频一次性取证已取消，未写入文件");
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_capturing = false;
        m_cancelRequested = false;
    }
}

void AudioDiagnosticCapture::setErrorLocked(const std::string& message)
{
    m_lastError = message;
}
