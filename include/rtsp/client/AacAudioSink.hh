#ifndef AAC_AUDIO_SINK_HH
#define AAC_AUDIO_SINK_HH

#include <MediaSink.hh>

#include "AudioTypes.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// live555 的 MPEG4-GENERIC RTP source 已负责把 AAC RTP 包还原成完整 access unit。
// 此 Sink 不做 AAC 解码，只把借用 payload、SDP 音频格式和 presentation time 同步交给上层。
class AacAudioSink : public MediaSink {
public:
    using AccessUnitCallback = std::function<void(const AudioEncodedPacket& packet)>;
    using ErrorCallback = std::function<void(const std::string& message)>;

    static AacAudioSink* createNew(UsageEnvironment& env,
                                   AudioPcmFormat sourceFormat,
                                   AccessUnitCallback accessUnitCallback,
                                   ErrorCallback errorCallback,
                                   unsigned maxAccessUnitBytes);

private:
    AacAudioSink(UsageEnvironment& env,
                 AudioPcmFormat sourceFormat,
                 AccessUnitCallback accessUnitCallback,
                 ErrorCallback errorCallback,
                 unsigned maxAccessUnitBytes);
    ~AacAudioSink() override;

    static void afterGettingFrame(void* clientData,
                                  unsigned frameSize,
                                  unsigned numTruncatedBytes,
                                  timeval presentationTime,
                                  unsigned durationInMicroseconds);
    void afterGettingFrame(unsigned frameSize,
                           unsigned numTruncatedBytes,
                           timeval presentationTime,
                           unsigned durationInMicroseconds);
    Boolean continuePlaying() override;

private:
    std::vector<uint8_t> m_receiveBuffer;
    AudioPcmFormat m_sourceFormat {};
    AccessUnitCallback m_accessUnitCallback;
    ErrorCallback m_errorCallback;
};

#endif // AAC_AUDIO_SINK_HH
