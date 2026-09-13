#include "StreamManager.hpp"

#include <limits>
#include <utility>
#include <vector>

StreamManager::StreamManager() = default;

StreamManager::~StreamManager()
{
    // 先停止所有 source/DecodeWorker，保证不会再有 Stream 回调访问 Manager。
    stopAllStreams();

    {
        std::lock_guard<std::mutex> lock(m_streamChangeMutex);
        for (auto& entry : m_streamMap) {
            if (entry.second.stream != nullptr) {
                entry.second.stream->setFrameReadyCallback({});
                entry.second.stream->setRuntimeStateCallback({});
            }
        }
        for (auto& entry : m_frameHubMap) {
            if (entry.second != nullptr) {
                entry.second->close();
            }
        }
        m_streamMap.clear();
        m_frameHubMap.clear();
    }

    shutdownPublishing();
}

int StreamManager::addStream(std::shared_ptr<Stream> stream)
{
    if (stream == nullptr) {
        setError("addStream 的 stream 不能为空");
        return -1;
    }

    std::lock_guard<std::mutex> lock(m_streamChangeMutex);
    const int streamId = allocateStreamIdLocked();
    if (streamId < 0) {
        return -1;
    }

    auto hub = std::make_shared<FrameHub>(streamId);
    stream->setStreamId(streamId);
    stream->setFrameReadyCallback([this] {
        notifyFrameReady();
    });
    stream->setRuntimeStateCallback([this, streamId](uint64_t generation,
                                                      Stream::RuntimeState state,
                                                      const std::string& message) {
        notifyStreamRuntimeState(streamId, generation, state, message);
    });

    StreamSlot slot;
    slot.stream = std::move(stream);
    slot.state = StreamState::Ready;
    m_streamMap.emplace(streamId, std::move(slot));
    m_frameHubMap.emplace(streamId, std::move(hub));
    clearError();
    return streamId;
}

bool StreamManager::delStream(int streamId)
{
    std::lock_guard<std::mutex> lock(m_streamChangeMutex);
    const auto streamIt = m_streamMap.find(streamId);
    if (streamIt == m_streamMap.end()) {
        setError("删除 Stream 失败：streamId 不存在: " + std::to_string(streamId));
        return false;
    }

    StreamSlot& slot = streamIt->second;
    bool stopped = true;
    slot.state = StreamState::Deleting;
    if (slot.stream != nullptr) {
        // 先断开新帧通知，再停止 source 和 DecodeWorker；这样 Manager 销毁后不会留下悬空回调。
        slot.stream->setFrameReadyCallback({});
        slot.stream->setRuntimeStateCallback({});
        if (!slot.stream->stop()) {
            stopped = false;
            slot.lastError = slot.stream->lastError();
            setError("删除 Stream 时停止失败 streamId=" + std::to_string(streamId)
                     + ": " + slot.lastError);
        }
        // 删除时不保留未发布裸帧；清队列会立即释放其 FrameLease。
        slot.stream->clearReadyFrames();
    }

    const auto hubIt = m_frameHubMap.find(streamId);
    if (hubIt != m_frameHubMap.end() && hubIt->second != nullptr) {
        // 发布线程即使持有旧快照，closed hub 也不会再向 Sink 转发帧。
        hubIt->second->close();
        m_frameHubMap.erase(hubIt);
    }
    m_streamMap.erase(streamIt);
    if (stopped) {
        clearError();
    }
    return stopped;
}

bool StreamManager::addFrameSink(int streamId, std::shared_ptr<Sink> sink)
{
    if (sink == nullptr) {
        setError("addFrameSink 的 sink 不能为空");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_streamChangeMutex);
    const auto hubIt = m_frameHubMap.find(streamId);
    if (hubIt == m_frameHubMap.end() || hubIt->second == nullptr) {
        setError("添加 FrameSink 失败：streamId 不存在: " + std::to_string(streamId));
        return false;
    }
    if (!hubIt->second->addSink(std::move(sink))) {
        setError("添加 FrameSink 失败 streamId=" + std::to_string(streamId)
                 + ": " + hubIt->second->lastError());
        return false;
    }
    clearError();
    return true;
}

bool StreamManager::startStream(int streamId)
{
    // 没有 publisher 时，readyQueue 会很快填满；启动 source 前确保消费者已运行，启动发布线程。
    startPublishing();

    std::lock_guard<std::mutex> lock(m_streamChangeMutex);
    const auto streamIt = m_streamMap.find(streamId);
    if (streamIt == m_streamMap.end() || streamIt->second.stream == nullptr) {
        setError("启动 Stream 失败：streamId 不存在: " + std::to_string(streamId));
        return false;
    }

    StreamSlot& slot = streamIt->second;
    if (slot.state == StreamState::Streaming) {
        return true;
    }
    if (slot.state == StreamState::Deleting) {
        setError("启动 Stream 失败：stream 正在删除: " + std::to_string(streamId));
        return false;
    }

    slot.runGeneration = nextRunGeneration(slot.runGeneration);
    slot.stream->setRunGeneration(slot.runGeneration);
    slot.stream->clearReadyFrames();
    slot.state = StreamState::Starting;
    slot.lastError.clear();

	//启动
    if (!slot.stream->start()) {
        slot.state = StreamState::Error;
        slot.lastError = slot.stream->lastError();
        setError("启动 Stream 失败 streamId=" + std::to_string(streamId)
                 + ": " + slot.lastError);
        return false;
    }

    // 对 RTSP 而言这里只代表事件线程已启动；PLAY 200 OK 后的异步状态回调才会改为 Streaming。
    clearError();
    return true;
}

bool StreamManager::stopStream(int streamId)
{
    std::lock_guard<std::mutex> lock(m_streamChangeMutex);
    const auto streamIt = m_streamMap.find(streamId);
    if (streamIt == m_streamMap.end() || streamIt->second.stream == nullptr) {
        setError("停止 Stream 失败：streamId 不存在: " + std::to_string(streamId));
        return false;
    }

    StreamSlot& slot = streamIt->second;
    if (slot.state == StreamState::Deleting) {
        setError("停止 Stream 失败：stream 正在删除: " + std::to_string(streamId));
        return false;
    }
    slot.state = StreamState::Stopping;
    slot.runGeneration = nextRunGeneration(slot.runGeneration);
    slot.stream->setRunGeneration(slot.runGeneration);
    if (!slot.stream->stop()) {
        slot.state = StreamState::Error;
        slot.lastError = slot.stream->lastError();
        setError("停止 Stream 失败 streamId=" + std::to_string(streamId)
                 + ": " + slot.lastError);
        return false;
    }

    // stop 后不应让下次 start() 发布停止前遗留的旧图像，同时归还对应 DMA pool buffer。
    slot.stream->clearReadyFrames();
    slot.state = StreamState::Stopped;
    slot.lastError.clear();
    clearError();
    return true;
}

bool StreamManager::startAllStreams()
{
    startPublishing();

    std::lock_guard<std::mutex> lock(m_streamChangeMutex);
    bool success = true;
    for (auto& entry : m_streamMap) {
        const int streamId = entry.first;
        StreamSlot& slot = entry.second;
        if (slot.stream == nullptr || slot.state == StreamState::Streaming ||
            slot.state == StreamState::Starting || slot.state == StreamState::Deleting) {
            continue;
        }
        slot.runGeneration = nextRunGeneration(slot.runGeneration);
        slot.stream->setRunGeneration(slot.runGeneration);
        slot.stream->clearReadyFrames();
        slot.state = StreamState::Starting;
        slot.lastError.clear();
        if (!slot.stream->start()) {
            success = false;
            slot.state = StreamState::Error;
            slot.lastError = slot.stream->lastError();
            setError("启动 Stream 失败 streamId=" + std::to_string(streamId)
                     + ": " + slot.lastError);
            continue;
        }
    }
    if (success) {
        clearError();
    }
    return success;
}

void StreamManager::stopAllStreams()
{
    std::lock_guard<std::mutex> lock(m_streamChangeMutex);
    for (auto& entry : m_streamMap) {
        StreamSlot& slot = entry.second;
        if (slot.stream == nullptr || slot.state == StreamState::Deleting) {
            continue;
        }
        slot.state = StreamState::Stopping;
        slot.runGeneration = nextRunGeneration(slot.runGeneration);
        slot.stream->setRunGeneration(slot.runGeneration);
        if (!slot.stream->stop()) {
            slot.state = StreamState::Error;
            slot.lastError = slot.stream->lastError();
            setError("停止 Stream 失败 streamId=" + std::to_string(entry.first)
                     + ": " + slot.lastError);
            continue;
        }
        slot.stream->clearReadyFrames();
        slot.state = StreamState::Stopped;
        slot.lastError.clear();
    }
}

void StreamManager::startPublishing()
{
    std::lock_guard<std::mutex> lock(m_publishMutex);
    if (m_running) {
        return;
    }

    // 正常 shutdown 会 join。这里额外回收已经退出但尚未 join 的线程，保证不会对一个
    // joinable std::thread 直接赋新线程而触发 std::terminate。m_running 为 false 表示旧
    // 发布线程已经走完退出路径，不会再等待 m_publishMutex。
    if (m_publishThread.joinable()) {
        if (m_publishThread.get_id() == std::this_thread::get_id()) {
            setError("发布线程不能在自身回调中重新启动");
            return;
        }
        m_publishThread.join();
    }

    m_stopRequested = false;
    m_hasPendingFrames = false;
    m_running = true;
    m_publishThread = std::thread(&StreamManager::runPublishLoop, this);
}

void StreamManager::shutdownPublishing()
{
    {
        std::lock_guard<std::mutex> lock(m_publishMutex);
        if (!m_running && !m_publishThread.joinable()) {
            return;
        }
        m_stopRequested = true;
    }
    m_publishCv.notify_one();

    // 正常应由 Manager 控制线程调用 shutdown。若未来某个 Sink 误在发布线程内调用，
    // 只能请求循环退出，不能 join 自己、更不能立即清掉 stop 标志。
    if (m_publishThread.joinable() && m_publishThread.get_id() == std::this_thread::get_id()) {
        return;
    }

    if (m_publishThread.joinable()) {
        m_publishThread.join();
    }

    std::lock_guard<std::mutex> lock(m_publishMutex);
    m_running = false;
    m_stopRequested = false;
    m_hasPendingFrames = false;
}

std::string StreamManager::lastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_lastError;
}

bool StreamManager::getStreamState(int streamId,
                                   StreamState& state,
                                   std::string* lastError) const
{
    std::lock_guard<std::mutex> lock(m_streamChangeMutex);
    const auto streamIt = m_streamMap.find(streamId);
    if (streamIt == m_streamMap.end()) {
        return false;
    }
    state = streamIt->second.state;
    if (lastError != nullptr) {
        *lastError = streamIt->second.lastError;
    }
    return true;
}

void StreamManager::runPublishLoop()
{
    while (true) {
        {
            std::unique_lock<std::mutex> lock(m_publishMutex);
            m_publishCv.wait(lock, [this] {
                return m_stopRequested || m_hasPendingFrames;
            });
            if (m_stopRequested) {
                break;
            }
            // 发布期间若有新帧到达，notifyFrameReady() 会再次置 true；下一轮不会睡眠。
            m_hasPendingFrames = false;
        }

        applyPendingStreamRuntimeStates();
        (void)publishReadyFrames();
    }

    std::lock_guard<std::mutex> lock(m_publishMutex);
    m_running = false;
}

bool StreamManager::publishReadyFrames()
{
    struct PublishTarget {
        std::shared_ptr<Stream> stream;
        std::shared_ptr<FrameHub> hub;
    };

    std::vector<PublishTarget> targets;
    {
        std::lock_guard<std::mutex> lock(m_streamChangeMutex);
        targets.reserve(m_streamMap.size());
        for (const auto& entry : m_streamMap) {
            const int streamId = entry.first;
            const StreamSlot& slot = entry.second;
            const auto hubIt = m_frameHubMap.find(streamId);
            if (slot.state == StreamState::Streaming && slot.stream != nullptr &&
                hubIt != m_frameHubMap.end() && hubIt->second != nullptr) {
                targets.push_back({slot.stream, hubIt->second});
            }
        }
    }

    bool publishedFrame = false;
    for (const PublishTarget& target : targets) {
        FramePacket packet;
        while (target.stream->tryGetFrame(packet)) {
            // FrameHub 只同步分发引用计数 lease；Sink 必须快速返回，不能在此阻塞。
            if (!target.hub->publishFrame(packet)) {
                setError("FrameHub 发布失败 streamId="
                         + std::to_string(target.stream->streamId())
                         + ": " + target.hub->lastError());
            }
            publishedFrame = true;
        }
    }
    return publishedFrame;
}

void StreamManager::notifyFrameReady()
{
    bool needWake = false;
    {
        std::lock_guard<std::mutex> lock(m_publishMutex);
        if (m_stopRequested) {
            return;
        }
        needWake = !m_hasPendingFrames;
        m_hasPendingFrames = true;
    }

    if (needWake) {
        m_publishCv.notify_one();
    }
}

void StreamManager::notifyStreamRuntimeState(int streamId,
                                              uint64_t generation,
                                              Stream::RuntimeState state,
                                              const std::string& message)
{
    bool needWake = false;
    {
        std::lock_guard<std::mutex> lock(m_publishMutex);
        if (m_stopRequested) {
            return;
        }
        m_pendingStreamRuntimeStates.push_back({streamId, generation, state, message});
        needWake = !m_hasPendingFrames;
        m_hasPendingFrames = true;
    }

    if (needWake) {
        m_publishCv.notify_one();
    }
}

//更新manager中的stream的状态
void StreamManager::applyPendingStreamRuntimeStates()
{
    std::deque<PendingStreamRuntimeState> pendingStates;
    {
        std::lock_guard<std::mutex> lock(m_publishMutex);
        pendingStates.swap(m_pendingStreamRuntimeStates);
    }

    if (pendingStates.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_streamChangeMutex);
    for (const PendingStreamRuntimeState& pending : pendingStates) {
        const auto streamIt = m_streamMap.find(pending.streamId);
        if (streamIt == m_streamMap.end()) {
            continue;
        }

        StreamSlot& slot = streamIt->second;
        // stop/restart 后才抵达的旧事件不能覆盖新一轮连接状态。
        if (slot.runGeneration != pending.generation || slot.state == StreamState::Deleting) {
            continue;
        }

        switch (pending.state) {
        case Stream::RuntimeState::Connecting:
            if (slot.state != StreamState::Stopping && slot.state != StreamState::Stopped) {
                slot.state = StreamState::Starting;
            }
            break;
        case Stream::RuntimeState::Streaming:
            // live555 PLAY 后也可能因解码恢复超时暂时进入 Error；当后续重新收到
            // 完整参数集 + IDR 时，DecodeWorker 会重新上报 Streaming，不需要重建 RTSP 会话。
            if (slot.state != StreamState::Stopping && slot.state != StreamState::Stopped) {
                slot.state = StreamState::Streaming;
                slot.lastError.clear();
                clearError();
            }
            break;
        case Stream::RuntimeState::Stopped:
            slot.stream->clearReadyFrames();
            slot.state = StreamState::Stopped;
            slot.lastError.clear();
            break;
        case Stream::RuntimeState::Error:
            slot.stream->clearReadyFrames();
            slot.state = StreamState::Error;
            slot.lastError = pending.message;
            setError("RTSP Stream 异步失败 streamId=" + std::to_string(pending.streamId)
                     + ": " + pending.message);
            break;
        }
    }
}

int StreamManager::allocateStreamIdLocked()
{
    if (m_nextStreamId < 0 || m_nextStreamId == std::numeric_limits<int>::max()) {
        setError("streamId 已耗尽");
        return -1;
    }
    return m_nextStreamId++;
}

uint64_t StreamManager::nextRunGeneration(uint64_t currentGeneration)
{
    // 0 仅用于尚未启动；溢出后回绕为 1，避免与默认状态混淆。
    return currentGeneration == std::numeric_limits<uint64_t>::max()
               ? 1
               : currentGeneration + 1;
}

void StreamManager::setError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError = message;
}

void StreamManager::clearError()
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError.clear();
}
