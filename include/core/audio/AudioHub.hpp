#pragma once

#include "AudioFrame.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

/*
 * 轻量的、进程内的音频发布/订阅 Hub。
 *
 * Hub 不拥有工作线程，也不做 APM/编码。publish() 在生产者线程执行，因此订阅回调只能
 * 做快速入队或计数，不能在这里做编码、网络发送、写盘等耗时操作。
 *
 * 回调接收 shared_ptr<const T>，因此多个支路共享同一份已拥有的数据；取消订阅与 publish
 * 并发时，允许当前已取出的回调再执行一次，Node 的 enqueue() 必须自行处理 stop 状态。
 */
template <typename T>
class AudioHub {
public:
    using ItemPtr = std::shared_ptr<const T>;
    using Callback = std::function<void(ItemPtr)>;

private:
    struct State {
        using CallbackSnapshot = std::vector<Callback>;

        State()
            : callbackSnapshot(std::make_shared<const CallbackSnapshot>())
        {
        }

        /* 必须在 mutex 已持有时调用。发布路径只读取这份不可变快照。 */
        void rebuildCallbackSnapshotLocked()
        {
            auto snapshot = std::make_shared<CallbackSnapshot>();
            snapshot->reserve(callbacks.size());
            for (const auto& entry : callbacks) {
                snapshot->push_back(entry.second);
            }
            callbackSnapshot = std::move(snapshot);
        }

        std::mutex mutex;
        std::mutex publishMutex;
        std::unordered_map<uint64_t, Callback> callbacks;
        std::shared_ptr<const CallbackSnapshot> callbackSnapshot;
        uint64_t nextSubscriptionId = 1;
    };

public:
    class Subscription {
    public:
        Subscription() = default;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        Subscription(Subscription&& other) noexcept
            : m_state(std::move(other.m_state))
            , m_subscriptionId(other.m_subscriptionId)
        {
            other.m_subscriptionId = 0;
        }

        Subscription& operator=(Subscription&& other) noexcept
        {
            if (this != &other) {
                reset();
                m_state = std::move(other.m_state);
                m_subscriptionId = other.m_subscriptionId;
                other.m_subscriptionId = 0;
            }
            return *this;
        }

        ~Subscription()
        {
            reset();
        }

        void reset()
        {
            if (!m_state || m_subscriptionId == 0) {
                return;
            }
            /* lock_guard 仍引用 mutex 时不能提前释放 State。 */
            const std::shared_ptr<State> state = m_state;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->callbacks.erase(m_subscriptionId) != 0) {
                    /* 订阅变更才复制 Callback；publish() 不再逐帧建 vector 快照。 */
                    state->rebuildCallbackSnapshotLocked();
                }
            }
            m_subscriptionId = 0;
            m_state.reset();
        }

        bool valid() const
        {
            return m_state != nullptr && m_subscriptionId != 0;
        }

    private:
        friend class AudioHub<T>;

        Subscription(std::shared_ptr<State> state, uint64_t subscriptionId)
            : m_state(std::move(state))
            , m_subscriptionId(subscriptionId)
        {
        }

        std::shared_ptr<State> m_state;
        uint64_t m_subscriptionId = 0;
    };

    AudioHub()
        : m_state(std::make_shared<State>())
    {
    }

    Subscription subscribe(Callback callback)
    {
        if (!callback) {
            return {};
        }

        std::lock_guard<std::mutex> lock(m_state->mutex);
        const uint64_t subscriptionId = m_state->nextSubscriptionId++;
        m_state->callbacks.emplace(subscriptionId, std::move(callback));
        /* 写时复制：订阅通常极少变化，换取 10ms publish 路径零 Callback 堆分配。 */
        m_state->rebuildCallbackSnapshotLocked();
        return Subscription(m_state, subscriptionId);
    }

    void publish(ItemPtr item)
    {
        if (!item) {
            return;
        }

        /* 一条 Hub 的生产顺序是时间线的一部分，不能让多个 publish 调用交叉回调。 */
        std::lock_guard<std::mutex> publishLock(m_state->publishMutex);
        std::shared_ptr<const typename State::CallbackSnapshot> callbacks;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            callbacks = m_state->callbackSnapshot;
        }

        for (const Callback& callback : *callbacks) {
            callback(item);
        }
    }

    size_t subscriberCount() const
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return m_state->callbacks.size();
    }

private:
    std::shared_ptr<State> m_state;
};

using AudioPcmHub = AudioHub<AudioFrame>;
using AudioEncodedPacketHub = AudioHub<EncodedAudioPacket>;
