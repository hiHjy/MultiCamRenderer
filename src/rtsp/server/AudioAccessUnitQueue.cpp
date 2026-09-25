#include "AudioAccessUnitQueue.hh"

#include <algorithm>
#include <utility>

AudioAccessUnitQueue::AudioAccessUnitQueue(size_t capacity)
    : m_capacity(capacity == 0 ? 1 : capacity)
{
}

bool AudioAccessUnitQueue::push(EncodedAudioPacketPtr packet)
{
    if (!packet || packet->codec != AUDIO_CODEC_AAC || packet->bytes.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    bool dropped = false;
    if (m_queue.size() == m_capacity) {
        m_queue.pop_front();
        ++m_droppedAccessUnits;
        dropped = true;
    }
    m_queue.push_back({std::move(packet)});
    wakeReadersLocked();
    return dropped;
}

bool AudioAccessUnitQueue::pop(AccessUnit& accessUnit)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty()) {
        return false;
    }
    accessUnit = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

void AudioAccessUnitQueue::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
}

void AudioAccessUnitQueue::registerReader(TaskScheduler* scheduler,
                                          EventTriggerId triggerId,
                                          void* clientData)
{
    if (scheduler == nullptr || triggerId == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_readers.push_back({scheduler, triggerId, clientData});
}

void AudioAccessUnitQueue::unregisterReader(EventTriggerId triggerId)
{
    if (triggerId == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_readers.erase(std::remove_if(m_readers.begin(), m_readers.end(),
                                   [triggerId](const Reader& reader) {
                                       return reader.triggerId == triggerId;
                                   }),
                    m_readers.end());
}

size_t AudioAccessUnitQueue::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

uint64_t AudioAccessUnitQueue::droppedAccessUnits() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_droppedAccessUnits;
}

void AudioAccessUnitQueue::wakeReadersLocked()
{
    for (const Reader& reader : m_readers) {
        reader.scheduler->triggerEvent(reader.triggerId, reader.clientData);
    }
}
