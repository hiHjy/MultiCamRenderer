#include "AacAudioSubsession.hh"

#include "AudioAccessUnitQueue.hh"
#include "AudioAccessUnitSource.hh"
#include "AudioAac.h"

#include <GroupsockHelper.hh>
#include <MPEG4GenericRTPSink.hh>

#include <array>
#include <cstdio>
#include <utility>

namespace {

std::string audioSpecificConfigToHex(const AudioPcmFormat& format)
{
    std::array<uint8_t, 2> config {};
    size_t configSize = config.size();
    if (audio_aac_lc_make_audio_specific_config(&format, config.data(), &configSize) < 0) {
        return {};
    }

    std::string hex(configSize * 2, '\0');
    for (size_t index = 0; index < configSize; ++index) {
        std::snprintf(hex.data() + index * 2, 3, "%02X", config[index]);
    }
    return hex;
}

} // namespace

AacAudioSubsession* AacAudioSubsession::createNew(UsageEnvironment& env,
                                                   std::shared_ptr<AudioAccessUnitQueue> queue,
                                                   AudioPcmFormat pcmFormat,
                                                   unsigned estimatedBitrateKbps,
                                                   std::function<void(bool)> clientPlaybackStateChanged)
{
    return new AacAudioSubsession(env, std::move(queue), pcmFormat, estimatedBitrateKbps,
                                  std::move(clientPlaybackStateChanged));
}

FramedSource* AacAudioSubsession::createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate)
{
    (void)clientSessionId;
    estBitrate = m_estimatedBitrateKbps;
    return AudioAccessUnitSource::createNew(envir(), m_queue);
}

RTPSink* AacAudioSubsession::createNewRTPSink(Groupsock* rtpGroupsock,
                                               unsigned char rtpPayloadTypeIfDynamic,
                                               FramedSource* inputSource)
{
    (void)inputSource;
    return MPEG4GenericRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
                                          m_pcmFormat.sampleRate, "audio", "AAC-hbr",
                                          m_audioSpecificConfigHex.c_str(), m_pcmFormat.channels);
}

void AacAudioSubsession::startStream(unsigned clientSessionId,
                                     void* streamToken,
                                     TaskFunc* rtcpRRHandler,
                                     void* rtcpRRHandlerClientData,
                                     unsigned short& rtpSeqNum,
                                     unsigned& rtpTimestamp,
                                     ServerRequestAlternativeByteHandler* serverRequestAlternativeByteHandler,
                                     void* serverRequestAlternativeByteHandlerClientData)
{
    if (m_activeClientSessionIds.insert(clientSessionId).second && m_clientPlaybackStateChanged) {
        m_clientPlaybackStateChanged(true);
    }
    OnDemandServerMediaSubsession::startStream(clientSessionId, streamToken, rtcpRRHandler,
                                                rtcpRRHandlerClientData, rtpSeqNum, rtpTimestamp,
                                                serverRequestAlternativeByteHandler,
                                                serverRequestAlternativeByteHandlerClientData);
}

void AacAudioSubsession::deleteStream(unsigned clientSessionId, void*& streamToken)
{
    if (m_activeClientSessionIds.erase(clientSessionId) != 0 && m_clientPlaybackStateChanged) {
        m_clientPlaybackStateChanged(false);
    }
    OnDemandServerMediaSubsession::deleteStream(clientSessionId, streamToken);
}

AacAudioSubsession::AacAudioSubsession(UsageEnvironment& env,
                                       std::shared_ptr<AudioAccessUnitQueue> queue,
                                       AudioPcmFormat pcmFormat,
                                       unsigned estimatedBitrateKbps,
                                       std::function<void(bool)> clientPlaybackStateChanged)
    : OnDemandServerMediaSubsession(env, True)
    , m_queue(std::move(queue))
    , m_pcmFormat(pcmFormat)
    , m_estimatedBitrateKbps(estimatedBitrateKbps == 0 ? 64 : estimatedBitrateKbps)
    , m_audioSpecificConfigHex(audioSpecificConfigToHex(pcmFormat))
    , m_clientPlaybackStateChanged(std::move(clientPlaybackStateChanged))
{
}

AacAudioSubsession::~AacAudioSubsession() = default;
