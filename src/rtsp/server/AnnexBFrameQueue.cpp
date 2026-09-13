#include "AnnexBFrameQueue.hh"

#include <algorithm>
#include <limits>

AnnexBFrameQueue::AnnexBFrameQueue() = default;

void AnnexBFrameQueue::pushAnnexBFrame(const uint8_t* data, size_t size, uint64_t timestampUs)
{
    if (data == nullptr || size == 0)
        return;

    const timeval presentationTime = timestampToTimeval(timestampUs);
    const std::vector<NaluSpan> nalus = splitAnnexB(data, size);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (nalus.empty()) {
        pushPureNaluLocked(data, size, presentationTime);
    } else {
        for (const NaluSpan& nalu : nalus) {
            pushPureNaluLocked(data + nalu.offset, nalu.size, presentationTime);
        }
    }
    wakeReadersLocked();
}

bool AnnexBFrameQueue::popNalu(Nalu& nalu)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty())
        return false;

    nalu = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

void AnnexBFrameQueue::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
}

void AnnexBFrameQueue::registerReader(TaskScheduler* scheduler,
                                      EventTriggerId triggerId,
                                      void* clientData)
{
    if (scheduler == nullptr || triggerId == 0)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_readers.push_back({scheduler, triggerId, clientData});
}

void AnnexBFrameQueue::unregisterReader(EventTriggerId triggerId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_readers.erase(std::remove_if(m_readers.begin(), m_readers.end(),
                                   [triggerId](const Reader& reader) {
                                       return reader.triggerId == triggerId;
                                   }),
                    m_readers.end());
}

size_t AnnexBFrameQueue::findStartCode(const uint8_t* data,
                                       size_t size,
                                       size_t from,
                                       size_t& startCodeSize)
{
    for (size_t i = from; i + 3 <= size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            startCodeSize = 3;
            return i;
        }
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            startCodeSize = 4;
            return i;
        }
    }

    startCodeSize = 0;
    return std::numeric_limits<size_t>::max();
}

std::vector<AnnexBFrameQueue::NaluSpan> AnnexBFrameQueue::splitAnnexB(const uint8_t* data, size_t size)
{
    const size_t noPosition = std::numeric_limits<size_t>::max();
    std::vector<NaluSpan> nalus;
    size_t startCodeSize = 0;
    size_t start = findStartCode(data, size, 0, startCodeSize);

    while (start != noPosition) {
        size_t nextStartCodeSize = 0;
        const size_t nextStart = findStartCode(data, size, start + startCodeSize, nextStartCodeSize);
        const size_t naluStart = start + startCodeSize;
        const size_t naluEnd = nextStart == noPosition ? size : nextStart;
        if (naluEnd > naluStart)
            nalus.push_back({naluStart, naluEnd - naluStart});
        start = nextStart;
        startCodeSize = nextStartCodeSize;
    }
    return nalus;
}

timeval AnnexBFrameQueue::timestampToTimeval(uint64_t timestampUs)
{
    return {static_cast<decltype(timeval::tv_sec)>(timestampUs / 1000000),
            static_cast<decltype(timeval::tv_usec)>(timestampUs % 1000000)};
}

void AnnexBFrameQueue::pushPureNaluLocked(const uint8_t* data,
                                          size_t size,
                                          const timeval& presentationTime)
{
    if (size == 0)
        return;

    while (m_queue.size() >= m_maxQueuedNalus)
        m_queue.pop_front();

    Nalu nalu;
    nalu.data.assign(data, data + size);
    nalu.presentationTime = presentationTime;
    m_queue.push_back(std::move(nalu));
}

void AnnexBFrameQueue::wakeReadersLocked()
{
    for (const Reader& reader : m_readers) {
        reader.scheduler->triggerEvent(reader.triggerId, reader.clientData);
    }
}
