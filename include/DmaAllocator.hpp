#pragma once

#include <cstddef>
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

private:
    int m_fd = -1;
    void* m_va = nullptr;
    size_t m_size = 0;
};

class DmaAllocator {
public:
    explicit DmaAllocator(std::string preferredHeapPath = "/dev/dma_heap/system");

    bool allocate(size_t size, DmaMemory& out);

    const std::string& lastError() const;

private:
    std::vector<std::string> heapCandidates() const;
    bool allocateFromHeap(const std::string& heapPath,
                          size_t size,
                          DmaMemory& out);
    void setError(const std::string& message);

private:
    std::string m_preferredHeapPath;
    std::string m_lastError;
};
