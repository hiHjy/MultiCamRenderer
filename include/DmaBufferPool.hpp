#pragma once

#include "DmaAllocator.hpp"
#include "VideoFrame.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

class DmaBufferPool {
public:
    enum class PoolState {
        // 尚未 init，或者 reset 后没有持有任何 DMA buffer。
        Uninitialized,
        // 已初始化，并且至少有一个空闲 buffer 可以 acquire。
        Ready,
        // 已初始化，但所有 buffer 都被借出去了。
        Busy,
        // 初始化或运行过程中遇到错误。
        Error
    };

    DmaBufferPool() = default;
    ~DmaBufferPool();

    DmaBufferPool(const DmaBufferPool&) = delete;
    DmaBufferPool& operator=(const DmaBufferPool&) = delete;

    DmaBufferPool(DmaBufferPool&&) = delete;
    DmaBufferPool& operator=(DmaBufferPool&&) = delete;

    // 这里只按 bufferSize 申请 DMA 内存，不绑定宽高、格式、stride。
    // 如果这个 pool 要给 RGA/MPP 做图像目标 buffer，调用方必须用对应模块的
    // stride/pitch 对齐规则先算好 bufferSize。比如 RGA 目标建议使用
    // RgaEngine::bufferSizeFor(format, width, height, 64)，而不是 visibleWidth * height * bpp。
    // 取出的 VideoFrame 只有 dmaFd/va/capacity/bufferIndex 是固定资源信息；
    // 图像 layout 由 RGA/MPP/V4L2 等真正写入图像的一方填写。
    bool init(int count,
              size_t bufferSize,
              const std::string& dmaHeapPath = "/dev/dma_heap/system");
    void reset();

    // acquire/release 只是“借出/归还 DMA buffer”。
    // 返回的 VideoFrame* 不是所有权转移，不能 close fd，也不能保存到 pool 析构之后。
    VideoFrame* acquireFrame();
    bool releaseFrame(VideoFrame* frame);

    bool initialized() const;
    PoolState state() const;
    static const char* stateName(PoolState state);
    size_t bufferSize() const;

    int totalCount() const;
    int freeCount() const;
    int usedCount() const;

    const std::string& lastError() const;

private:
    struct Slot {
        DmaMemory memory;
        VideoFrame frame;
        bool inUse = false;
    };

    Slot* findSlotByFrameLocked(VideoFrame* frame);
    int freeCountLocked() const;
    void resetLocked();
    void updateStateAfterSlotChangeLocked();
    void setErrorLocked(const std::string& message, PoolState state);

private:
    mutable std::mutex m_mutex;
    std::vector<Slot> m_slots;
    size_t m_bufferSize = 0;
    PoolState m_state = PoolState::Uninitialized;
    std::string m_lastError;
};
