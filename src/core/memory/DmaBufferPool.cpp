#include "DmaBufferPool.hpp"

#include <utility>

DmaBufferPool::~DmaBufferPool()
{
    reset();
}

bool DmaBufferPool::init(int count,
                        size_t bufferSize,
                        const std::string& dmaHeapPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    resetLocked();

    if (count <= 0) {
        setErrorLocked("DmaBufferPool count 必须大于 0", PoolState::Error);
        return false;
    }

    if (bufferSize == 0) {
        setErrorLocked("DmaBufferPool bufferSize 不能为 0", PoolState::Error);
        return false;
    }

    DmaAllocator allocator(dmaHeapPath);
    m_slots.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
        Slot slot {};
        if (!allocator.allocate(bufferSize, slot.memory)) {
            resetLocked();
            setErrorLocked("DmaBufferPool 分配 DMA buffer 失败: " + allocator.lastError(), PoolState::Error);
            return false;
        }

        slot.frame.dmaFd = slot.memory.fd();
        slot.frame.va = slot.memory.va();
        slot.frame.capacity = slot.memory.size();
        slot.frame.bytesUsed = 0;
        slot.frame.bufferIndex = i;
        slot.inUse = false;

        m_slots.push_back(std::move(slot));
    }

    m_bufferSize = bufferSize;
    m_state = PoolState::Ready;
    m_lastError.clear();
    return true;
}

void DmaBufferPool::reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    resetLocked();
}

void DmaBufferPool::resetLocked()
{
    m_slots.clear();
    m_bufferSize = 0;
    m_state = PoolState::Uninitialized;
    m_lastError.clear();
}

VideoFrame* DmaBufferPool::acquireFrame()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state == PoolState::Uninitialized) {
        setErrorLocked("DmaBufferPool 尚未初始化", PoolState::Uninitialized);
        return nullptr;
    }

    if (m_state == PoolState::Error) {
        setErrorLocked("DmaBufferPool 处于错误状态", PoolState::Error);
        return nullptr;
    }

    for (Slot& slot : m_slots) {
        if (!slot.inUse) {
            slot.inUse = true;
            slot.frame.streamId = -1;
            slot.frame.bytesUsed = 0;
            slot.frame.width = 0;
            slot.frame.height = 0;
            slot.frame.stride = 0;
            slot.frame.heightStride = 0;
            slot.frame.format = PixelFormat::Unknown;
            slot.frame.nativeFormat = 0;
            slot.frame.timestampUs = 0;
            slot.frame.sequence = 0;
            updateStateAfterSlotChangeLocked();
            m_lastError.clear();
            return &slot.frame;
        }
    }

    setErrorLocked("DmaBufferPool 没有空闲帧", PoolState::Busy);
    return nullptr;
}

bool DmaBufferPool::releaseFrame(VideoFrame* frame)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (frame == nullptr) {
        setErrorLocked("releaseFrame frame 为空", m_state);
        return false;
    }

    Slot* slot = findSlotByFrameLocked(frame);
    if (slot == nullptr) {
        setErrorLocked("releaseFrame frame 不属于当前 DmaBufferPool", m_state);
        return false;
    }

    if (!slot->inUse) {
        setErrorLocked("releaseFrame 重复释放 frame", m_state);
        return false;
    }

    slot->inUse = false;
    slot->frame.streamId = -1;
    slot->frame.bytesUsed = 0;
    slot->frame.width = 0;
    slot->frame.height = 0;
    slot->frame.stride = 0;
    slot->frame.heightStride = 0;
    slot->frame.format = PixelFormat::Unknown;
    slot->frame.nativeFormat = 0;
    slot->frame.timestampUs = 0;
    slot->frame.sequence = 0;
    updateStateAfterSlotChangeLocked();
    m_lastError.clear();
    return true;
}

bool DmaBufferPool::initialized() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state != PoolState::Uninitialized && m_state != PoolState::Error && !m_slots.empty();
}

DmaBufferPool::PoolState DmaBufferPool::state() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

const char* DmaBufferPool::stateName(PoolState state)
{
    switch (state) {
    case PoolState::Uninitialized:
        return "Uninitialized";
    case PoolState::Ready:
        return "Ready";
    case PoolState::Busy:
        return "Busy";
    case PoolState::Error:
        return "Error";
    }
    return "Unknown";
}

size_t DmaBufferPool::bufferSize() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_bufferSize;
}

int DmaBufferPool::totalCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_slots.size());
}

int DmaBufferPool::freeCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return freeCountLocked();
}

int DmaBufferPool::freeCountLocked() const
{
    int count = 0;
    for (const Slot& slot : m_slots) {
        if (!slot.inUse) {
            count++;
        }
    }
    return count;
}

int DmaBufferPool::usedCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_slots.size()) - freeCountLocked();
}

const std::string& DmaBufferPool::lastError() const
{
    return m_lastError;
}

DmaBufferPool::Slot* DmaBufferPool::findSlotByFrameLocked(VideoFrame* frame)
{
    for (Slot& slot : m_slots) {
        if (&slot.frame == frame) {
            return &slot;
        }
    }
    return nullptr;
}

void DmaBufferPool::updateStateAfterSlotChangeLocked()
{
    if (m_slots.empty()) {
        m_state = PoolState::Uninitialized;
        return;
    }

    m_state = freeCountLocked() == 0 ? PoolState::Busy : PoolState::Ready;
}

void DmaBufferPool::setErrorLocked(const std::string& message, PoolState state)
{
    m_lastError = message;
    m_state = state;
}
