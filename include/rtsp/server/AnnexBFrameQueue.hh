#pragma once

#include <BasicUsageEnvironment.hh>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <sys/time.h>
#include <vector>

// 线程安全、与编码格式无关的 Annex-B NALU 队列。H264/H265 使用相同的
// Annex-B 起始码；入队时拆成不含起始码的单个 NALU，交给 live555 RTP sink。
class AnnexBFrameQueue {
public:
    struct Nalu {
        std::vector<uint8_t> data;
        timeval presentationTime {};
    };

    AnnexBFrameQueue();

    void pushAnnexBFrame(const uint8_t* data, size_t size, uint64_t timestampUs);
    bool popNalu(Nalu& nalu);
    void clear();

    void registerReader(TaskScheduler* scheduler, EventTriggerId triggerId, void* clientData);
    void unregisterReader(EventTriggerId triggerId);

private:
    struct Reader {
        TaskScheduler* scheduler = nullptr;
        EventTriggerId triggerId = 0;
        void* clientData = nullptr;
    };

    struct NaluSpan {
        size_t offset = 0;
        size_t size = 0;
    };

    static size_t findStartCode(const uint8_t* data, size_t size, size_t from, size_t& startCodeSize);
    static std::vector<NaluSpan> splitAnnexB(const uint8_t* data, size_t size);
    static timeval timestampToTimeval(uint64_t timestampUs);

    void pushPureNaluLocked(const uint8_t* data, size_t size, const timeval& presentationTime);
    void wakeReadersLocked();

private:
    std::mutex m_mutex;
    std::deque<Nalu> m_queue;
    std::vector<Reader> m_readers;
    size_t m_maxQueuedNalus = 300;
};
