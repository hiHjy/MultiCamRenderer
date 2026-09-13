#include "AnnexBSource.hh"

#include <cstring>
#include <utility>

namespace {

// live555 先把完整 NALU 放入 OutPacketBuffer，再按 RTP MTU 分成 FU-A/FU。
constexpr unsigned kMaxVideoNaluBytes = 8U * 1024U * 1024U;

} // namespace

AnnexBSource* AnnexBSource::createNew(UsageEnvironment& env,
                                      std::shared_ptr<AnnexBFrameQueue> frameQueue)
{
    return new AnnexBSource(env, std::move(frameQueue));
}

AnnexBSource::AnnexBSource(UsageEnvironment& env, std::shared_ptr<AnnexBFrameQueue> frameQueue)
    : FramedSource(env),
      m_frameQueue(std::move(frameQueue))
{
    fMaxSize = kMaxVideoNaluBytes;
    m_frameArrivedTrigger = envir().taskScheduler().createEventTrigger(frameArrivedCallback);
    if (m_frameQueue != nullptr) {
        m_frameQueue->registerReader(&envir().taskScheduler(), m_frameArrivedTrigger, this);
    }
}

AnnexBSource::~AnnexBSource()
{
    doStopGettingFrames();
    if (m_frameQueue != nullptr)
        m_frameQueue->unregisterReader(m_frameArrivedTrigger);
    if (m_frameArrivedTrigger != 0) {
        envir().taskScheduler().deleteEventTrigger(m_frameArrivedTrigger);
        m_frameArrivedTrigger = 0;
    }
}

void AnnexBSource::doGetNextFrame()
{
    deliverFrame();
}

void AnnexBSource::deliverFrame()
{
    if (!isCurrentlyAwaitingData() || m_frameQueue == nullptr)
        return;

    AnnexBFrameQueue::Nalu nalu;
    if (!m_frameQueue->popNalu(nalu))
        return;

    const size_t sourceSize = nalu.data.size();
    fFrameSize = sourceSize > fMaxSize ? fMaxSize : static_cast<unsigned>(sourceSize);
    fNumTruncatedBytes = sourceSize > fMaxSize ? sourceSize - fMaxSize : 0;
    std::memcpy(fTo, nalu.data.data(), fFrameSize);
    fPresentationTime = nalu.presentationTime;
    // 上游以源帧真实时间戳驱动；live555 的 RTP sink 不需要额外帧率节流。
    fDurationInMicroseconds = 0;

    FramedSource::afterGetting(this);
}

void AnnexBSource::frameArrivedCallback(void* clientData)
{
    static_cast<AnnexBSource*>(clientData)->deliverFrame();
}
