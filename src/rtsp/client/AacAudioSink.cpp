#include "AacAudioSink.hh"

#include <utility>

AacAudioSink* AacAudioSink::createNew(UsageEnvironment& env,
                                      AudioPcmFormat sourceFormat,
                                      AccessUnitCallback accessUnitCallback,
                                      ErrorCallback errorCallback,
                                      unsigned maxAccessUnitBytes)
{
    if (!accessUnitCallback || sourceFormat.sampleRate == 0 || sourceFormat.channels == 0
        || maxAccessUnitBytes == 0) {
        return nullptr;
    }
    return new AacAudioSink(env, sourceFormat, std::move(accessUnitCallback), std::move(errorCallback),
                            maxAccessUnitBytes);
}

AacAudioSink::AacAudioSink(UsageEnvironment& env,
                           AudioPcmFormat sourceFormat,
                           AccessUnitCallback accessUnitCallback,
                           ErrorCallback errorCallback,
                           unsigned maxAccessUnitBytes)
    : MediaSink(env)
    , m_receiveBuffer(maxAccessUnitBytes)
    , m_sourceFormat(sourceFormat)
    , m_accessUnitCallback(std::move(accessUnitCallback))
    , m_errorCallback(std::move(errorCallback))
{
}

AacAudioSink::~AacAudioSink() = default;

void AacAudioSink::afterGettingFrame(void* clientData,
                                     unsigned frameSize,
                                     unsigned numTruncatedBytes,
                                     timeval presentationTime,
                                     unsigned durationInMicroseconds)
{
    static_cast<AacAudioSink*>(clientData)->afterGettingFrame(
        frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

void AacAudioSink::afterGettingFrame(unsigned frameSize,
                                     unsigned numTruncatedBytes,
                                     timeval presentationTime,
                                     unsigned /*durationInMicroseconds*/)
{
    if (numTruncatedBytes != 0) {
        const std::string message = "接收缓冲区不足，丢弃截断 AAC access unit：保留 "
            + std::to_string(frameSize) + " 字节，丢失 " + std::to_string(numTruncatedBytes) + " 字节";
        envir() << message.c_str() << "\n";
        if (m_errorCallback) {
            m_errorCallback(message);
        }
    } else {
        AudioEncodedPacket packet {};
        packet.codec = AUDIO_CODEC_AAC;
        packet.data = m_receiveBuffer.data();
        packet.size = frameSize;
        packet.timestampUs = presentationTime.tv_sec < 0
            ? 0
            : static_cast<uint64_t>(presentationTime.tv_sec) * 1000000ULL
                + static_cast<uint64_t>(presentationTime.tv_usec);
        // AAC-LC 的一个 raw_data_block 固定覆盖 1024 个每声道 samples。当前项目服务端
        // 只发布 AAC-LC；以后支持 HE-AAC/多 AU RTP 时，应在这里按 SDP/AU header 扩展。
        packet.frameSamples = 1024;
        packet.durationUs = m_sourceFormat.sampleRate == 0
            ? 0
            : static_cast<uint32_t>(1024ULL * 1000000ULL / m_sourceFormat.sampleRate);
        packet.sourceFormat = m_sourceFormat;
        m_accessUnitCallback(packet);
    }

    continuePlaying();
}

Boolean AacAudioSink::continuePlaying()
{
    if (fSource == nullptr) {
        return False;
    }
    fSource->getNextFrame(m_receiveBuffer.data(),
                          static_cast<unsigned>(m_receiveBuffer.size()),
                          afterGettingFrame,
                          this,
                          onSourceClosure,
                          this);
    return True;
}
