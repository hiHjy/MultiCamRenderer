#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class DmaMemory {
public:
    DmaMemory() = default;
    ~DmaMemory();

    DmaMemory(const DmaMemory&) = delete;
    DmaMemory& operator=(const DmaMemory&) = delete;

    DmaMemory(DmaMemory&& other) noexcept;
    DmaMemory& operator=(DmaMemory&& other) noexcept;

    int fd() const;
    void* va() const;
    size_t size() const;
    bool valid() const;

    void reset();

private:
    friend class DmaAllocator;

    DmaMemory(int fd, void* va, size_t size);
    DmaMemory(int fd, void* va, size_t size, int drmFd, uint32_t drmHandle);

private:
    int m_fd = -1;
    void* m_va = nullptr;
    size_t m_size = 0;
    int m_drmFd = -1;
    uint32_t m_drmHandle = 0;
};

class DmaAllocator {
public:
    explicit DmaAllocator(std::string preferredHeapPath = "/dev/dma_heap/system");

    // 只按字节数申请 DMA-BUF。图像 stride/pitch 对齐不是 allocator 的职责，
    // 调用方必须先按硬件模块要求计算好 size。
    // 分配顺序：
    // 1. 优先尝试 /dev/dma_heap，先试 preferredHeapPath，再试目录下其他 heap。
    // 2. dma_heap 全失败后，尝试 /dev/dri/card*、/dev/dri/renderD* 的 DRM dumb buffer，
    //    并导出 PRIME fd 作为 DMA-BUF。
    bool allocate(size_t size, DmaMemory& out);

    const std::string& lastError() const;

private:
    std::vector<std::string> heapCandidates() const;
    std::vector<std::string> drmCandidates() const;
    bool allocateFromHeap(const std::string& heapPath,
                          size_t size,
                          DmaMemory& out);
    bool allocateFromDrm(const std::string& drmPath,
                         size_t size,
                         DmaMemory& out);
    void setError(const std::string& message);

private:
    std::string m_preferredHeapPath;
    std::string m_lastError;
};
