#pragma once

#include "AudioFrame.hpp"

#include <BasicUsageEnvironment.hh>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

/*
 * 一个 RTSP audio track 的实时 AAC access-unit 队列。
 *
 * 队列项只保存 EncodedAudioPacketPtr：/main 与 /sub 各有自己的 queue，但两边共同引用
 * AudioPipeline 编出的同一份不可变 AAC payload，不复制压缩字节。每个 queue 仅供其所属
 * AudioSubsession 读取；同一路的多个客户端由 live555 的 reuseFirstSource 复用该 source。
 */
class AudioAccessUnitQueue {
public:
    struct AccessUnit {
        EncodedAudioPacketPtr packet;
    };

    explicit AudioAccessUnitQueue(size_t capacity = 8);

    /*
     * 推入一整包 AAC access unit。满时淘汰最旧包追实时；AAC-LC 各包独立，丢包不会像
     * H264/H265 丢 slice 一样破坏后续参考链。返回 true 表示本次淘汰过旧包。
     */
    bool push(EncodedAudioPacketPtr packet);
    bool pop(AccessUnit& accessUnit);
    void clear();

    /* AudioSource 在 live555 event-loop 中登记自己；生产者线程 push 后触发它重新取包。 */
    void registerReader(TaskScheduler* scheduler, EventTriggerId triggerId, void* clientData);
    void unregisterReader(EventTriggerId triggerId);

    size_t size() const;
    uint64_t droppedAccessUnits() const;

private:
    struct Reader {
        TaskScheduler* scheduler = nullptr;
        EventTriggerId triggerId = 0;
        void* clientData = nullptr;
    };

    void wakeReadersLocked();

private:
    const size_t m_capacity;
    mutable std::mutex m_mutex;
    std::vector<AccessUnit> m_queue;
    std::vector<Reader> m_readers;
    uint64_t m_droppedAccessUnits = 0;
};
