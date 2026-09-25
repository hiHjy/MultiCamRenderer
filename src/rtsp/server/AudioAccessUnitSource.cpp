#include "AudioAccessUnitSource.hh"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {

// 当前 AAC EncoderNode 的 maxPacketBytes=2048；留出余量，避免将来提高码率时 source 截断 AU。
constexpr unsigned kMaxAacAccessUnitBytes = 16U * 1024U;

} // namespace

AudioAccessUnitSource* AudioAccessUnitSource::createNew(UsageEnvironment& env,
                                                         std::shared_ptr<AudioAccessUnitQueue> queue)
{
    return new AudioAccessUnitSource(env, std::move(queue));
}

AudioAccessUnitSource::AudioAccessUnitSource(UsageEnvironment& env,
                                             std::shared_ptr<AudioAccessUnitQueue> queue)
    : FramedSource(env)
    , m_queue(std::move(queue))
{
    fMaxSize = kMaxAacAccessUnitBytes;
    m_accessUnitArrivedTrigger = envir().taskScheduler().createEventTrigger(accessUnitArrivedCallback);
    if (m_queue != nullptr) {
        m_queue->registerReader(&envir().taskScheduler(), m_accessUnitArrivedTrigger, this);
    }
}

AudioAccessUnitSource::~AudioAccessUnitSource()
{
    doStopGettingFrames();
    if (m_queue != nullptr) {
        m_queue->unregisterReader(m_accessUnitArrivedTrigger);
    }
    if (m_accessUnitArrivedTrigger != 0) {
        envir().taskScheduler().deleteEventTrigger(m_accessUnitArrivedTrigger);
        m_accessUnitArrivedTrigger = 0;
    }
}

void AudioAccessUnitSource::doGetNextFrame()
{
    deliverAccessUnit();
}

void AudioAccessUnitSource::deliverAccessUnit()
{
    if (!isCurrentlyAwaitingData() || m_queue == nullptr) {
        return;
    }

    AudioAccessUnitQueue::AccessUnit accessUnit;
    if (!m_queue->pop(accessUnit) || !accessUnit.packet) {
        return;
    }

    const EncodedAudioPacket& packet = *accessUnit.packet;
    const size_t sourceSize = packet.bytes.size();
    fFrameSize = sourceSize > fMaxSize ? fMaxSize : static_cast<unsigned>(sourceSize);
    fNumTruncatedBytes = sourceSize > fMaxSize ? static_cast<unsigned>(sourceSize - fMaxSize) : 0;
    std::memcpy(fTo, packet.bytes.data(), fFrameSize);
    fPresentationTime = {
        static_cast<decltype(timeval::tv_sec)>(packet.timestampUs / 1000000ULL),
        static_cast<decltype(timeval::tv_usec)>(packet.timestampUs % 1000000ULL),
    };
    fDurationInMicroseconds = packet.sourceFormat.sampleRate == 0
        ? 0
        : static_cast<unsigned>((uint64_t{packet.frameSamples} * 1000000ULL)
                                / packet.sourceFormat.sampleRate);

    FramedSource::afterGetting(this);
}

void AudioAccessUnitSource::accessUnitArrivedCallback(void* clientData)
{
    static_cast<AudioAccessUnitSource*>(clientData)->deliverAccessUnit();
}
