#pragma once

#include <OnDemandServerMediaSubsession.hh>

#include "AudioTypes.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>

class AudioAccessUnitQueue;

/*
 * 一个 RTSP URL 下的 AAC-LC audio track。
 *
 * 与 VideoSubsession 一样，reuseFirstSource=true 表示同一路 URL 的多个客户端共享同一个
 * 实时 AAC source，而不是彼此抢 AudioAccessUnitQueue。它只报告“本 audio track 是否在
 * PLAY”，不会触碰视频 RtspPublishSink 的编码器状态。
 */
class AacAudioSubsession : public OnDemandServerMediaSubsession {
public:
    static AacAudioSubsession* createNew(UsageEnvironment& env,
                                         std::shared_ptr<AudioAccessUnitQueue> queue,
                                         AudioPcmFormat pcmFormat,
                                         unsigned estimatedBitrateKbps,
                                         std::function<void(bool)> clientPlaybackStateChanged);

protected:
    FramedSource* createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate) override;
    RTPSink* createNewRTPSink(Groupsock* rtpGroupsock,
                              unsigned char rtpPayloadTypeIfDynamic,
                              FramedSource* inputSource) override;
    void startStream(unsigned clientSessionId,
                     void* streamToken,
                     TaskFunc* rtcpRRHandler,
                     void* rtcpRRHandlerClientData,
                     unsigned short& rtpSeqNum,
                     unsigned& rtpTimestamp,
                     ServerRequestAlternativeByteHandler* serverRequestAlternativeByteHandler,
                     void* serverRequestAlternativeByteHandlerClientData) override;
    void deleteStream(unsigned clientSessionId, void*& streamToken) override;

private:
    AacAudioSubsession(UsageEnvironment& env,
                       std::shared_ptr<AudioAccessUnitQueue> queue,
                       AudioPcmFormat pcmFormat,
                       unsigned estimatedBitrateKbps,
                       std::function<void(bool)> clientPlaybackStateChanged);
    ~AacAudioSubsession() override;

private:
    std::shared_ptr<AudioAccessUnitQueue> m_queue;
    AudioPcmFormat m_pcmFormat {};
    unsigned m_estimatedBitrateKbps = 64;
    std::string m_audioSpecificConfigHex;
    std::function<void(bool)> m_clientPlaybackStateChanged;
    std::unordered_set<unsigned> m_activeClientSessionIds;
};
