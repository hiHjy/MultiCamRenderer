#include "VideoSubsession.hh"

#include "AnnexBFrameQueue.hh"
#include "AnnexBSource.hh"

#include <H264VideoRTPSink.hh>
#include <H264VideoStreamDiscreteFramer.hh>
#include <H265VideoRTPSink.hh>
#include <H265VideoStreamDiscreteFramer.hh>
#include <GroupsockHelper.hh>

namespace {

constexpr unsigned kMaxVideoNaluBytes = 8U * 1024U * 1024U;
// H265 IDR 会被拆成大量 RTP FU。目标板默认 208KiB 的 UDP 发送缓冲太小，
// 在一个大 IDR 的突发发送时容易触发内核队列压力。
constexpr unsigned kUdpRtpSendBufferBytes = 4U * 1024U * 1024U;

} // namespace

VideoSubsession* VideoSubsession::createNew(UsageEnvironment& env,
                                            std::shared_ptr<AnnexBFrameQueue> frameQueue,
                                            VideoCodec codec,
                                            std::function<void(bool)> clientPlaybackStateChanged)
{
    return new VideoSubsession(env, std::move(frameQueue), codec,
                               std::move(clientPlaybackStateChanged));
}

FramedSource* VideoSubsession::createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate)
{
    (void)clientSessionId;
    estBitrate = 500;

    FramedSource* source = AnnexBSource::createNew(envir(), m_frameQueue);
    if (source == nullptr)
        return nullptr;

    return m_codec == VideoCodec::H264
        ? static_cast<FramedSource*>(H264VideoStreamDiscreteFramer::createNew(envir(), source))
        : static_cast<FramedSource*>(H265VideoStreamDiscreteFramer::createNew(envir(), source));
}

RTPSink* VideoSubsession::createNewRTPSink(Groupsock* rtpGroupsock,
                                            unsigned char rtpPayloadTypeIfDynamic,
                                            FramedSource* inputSource)
{
    (void)inputSource;
    OutPacketBuffer::maxSize = kMaxVideoNaluBytes;

    if (rtpGroupsock != nullptr) {
        const int socket = rtpGroupsock->socketNum();
        increaseSendBufferTo(envir(), socket, kUdpRtpSendBufferBytes);
    }

    return m_codec == VideoCodec::H264
        ? static_cast<RTPSink*>(H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic))
        : static_cast<RTPSink*>(H265VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic));
}

void VideoSubsession::startStream(unsigned clientSessionId,
                                  void* streamToken,
                                  TaskFunc* rtcpRRHandler,
                                  void* rtcpRRHandlerClientData,
                                  unsigned short& rtpSeqNum,
                                  unsigned& rtpTimestamp,
                                  ServerRequestAlternativeByteHandler* serverRequestAlternativeByteHandler,
                                  void* serverRequestAlternativeByteHandlerClientData)
{
    // 先开放上游入队，再由 live555 开始请求数据，避免刚 PLAY 时丢掉首个 IDR。
    if (m_activeClientSessionIds.insert(clientSessionId).second && m_clientPlaybackStateChanged) {
        m_clientPlaybackStateChanged(true);
    }

    OnDemandServerMediaSubsession::startStream(clientSessionId,
                                                streamToken,
                                                rtcpRRHandler,
                                                rtcpRRHandlerClientData,
                                                rtpSeqNum,
                                                rtpTimestamp,
                                                serverRequestAlternativeByteHandler,
                                                serverRequestAlternativeByteHandlerClientData);
}

void VideoSubsession::deleteStream(unsigned clientSessionId, void*& streamToken)
{
    if (m_activeClientSessionIds.erase(clientSessionId) != 0 && m_clientPlaybackStateChanged) {
        m_clientPlaybackStateChanged(false);
    }
    OnDemandServerMediaSubsession::deleteStream(clientSessionId, streamToken);
}

VideoSubsession::VideoSubsession(UsageEnvironment& env,
                                 std::shared_ptr<AnnexBFrameQueue> frameQueue,
                                 VideoCodec codec,
                                 std::function<void(bool)> clientPlaybackStateChanged)
    : OnDemandServerMediaSubsession(env, True),
      m_frameQueue(std::move(frameQueue)),
      m_codec(codec),
      m_clientPlaybackStateChanged(std::move(clientPlaybackStateChanged))
{
}

VideoSubsession::~VideoSubsession() = default;
