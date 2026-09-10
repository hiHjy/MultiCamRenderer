#pragma once

#include <OnDemandServerMediaSubsession.hh>

#include "VideoCodec.hpp"

#include <functional>
#include <memory>
#include <unordered_set>

class AnnexBFrameQueue;

// 一个 RTSP URL 下的视频 track。reuseFirstSource=true 让 live555 复用实时源，
// 多个客户端拉同一路 URL 时共享同一份实时编码输入，而不是各自消费队列。
class VideoSubsession : public OnDemandServerMediaSubsession {
public:
    static VideoSubsession* createNew(UsageEnvironment& env,
                                      std::shared_ptr<AnnexBFrameQueue> frameQueue,
                                      VideoCodec codec,
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
    VideoSubsession(UsageEnvironment& env,
                    std::shared_ptr<AnnexBFrameQueue> frameQueue,
                    VideoCodec codec,
                    std::function<void(bool)> clientPlaybackStateChanged);
    ~VideoSubsession() override;

private:
    std::shared_ptr<AnnexBFrameQueue> m_frameQueue;
    VideoCodec m_codec;
    std::function<void(bool)> m_clientPlaybackStateChanged;
    std::unordered_set<unsigned> m_activeClientSessionIds;
};
