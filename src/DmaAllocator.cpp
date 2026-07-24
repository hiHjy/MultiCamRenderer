#include "DmaAllocator.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

namespace {

constexpr const char* kDmaHeapDir = "/dev/dma_heap";

struct DmaHeapAllocationData {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};

#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC \
    _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, DmaHeapAllocationData)

int ioctlRetry(int fd, unsigned long request, void* arg)
{
    int ret = -1;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

std::string errnoText(const std::string& prefix)
{
    std::ostringstream oss;
    oss << prefix << ": " << std::strerror(errno);
    return oss.str();
}

} // namespace

DmaMemory::DmaMemory(int fd, void* va, size_t size)
    : m_fd(fd)
    , m_va(va)
    , m_size(size)
{
}

DmaMemory::~DmaMemory()
{
    reset();
}

DmaMemory::DmaMemory(DmaMemory&& other) noexcept
    : m_fd(other.m_fd)
    , m_va(other.m_va)
    , m_size(other.m_size)
{
    other.m_fd = -1;
    other.m_va = nullptr;
    other.m_size = 0;
}

DmaMemory& DmaMemory::operator=(DmaMemory&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    reset();
    m_fd = other.m_fd;
    m_va = other.m_va;
    m_size = other.m_size;

    other.m_fd = -1;
    other.m_va = nullptr;
    other.m_size = 0;
    return *this;
}

int DmaMemory::fd() const
{
    return m_fd;
}

void* DmaMemory::va() const
{
    return m_va;
}

size_t DmaMemory::size() const
{
    return m_size;
}

bool DmaMemory::valid() const
{
    return m_fd >= 0 && m_va != nullptr && m_va != MAP_FAILED && m_size > 0;
}

void DmaMemory::reset()
{
    if (m_va != nullptr && m_va != MAP_FAILED) {
        munmap(m_va, m_size);
    }

    if (m_fd >= 0) {
        close(m_fd);
    }

    m_fd = -1;
    m_va = nullptr;
    m_size = 0;
}

DmaAllocator::DmaAllocator(std::string preferredHeapPath)
    : m_preferredHeapPath(std::move(preferredHeapPath))
{
}

bool DmaAllocator::allocate(size_t size, DmaMemory& out)
{
    if (size == 0) {
        setError("DMA 分配大小不能为 0");
        return false;
    }

    out.reset();

    const std::vector<std::string> heaps = heapCandidates();
    if (heaps.empty()) {
        setError("未找到 /dev/dma_heap，暂不使用 DRM buffer fallback");
        return false;
    }

    std::string lastAllocError;
    for (const std::string& heap : heaps) {
        if (allocateFromHeap(heap, size, out)) {
            m_lastError.clear();
            return true;
        }
        lastAllocError = m_lastError;
    }

    setError(lastAllocError.empty() ? "DMA heap 分配失败" : lastAllocError);
    return false;
}

const std::string& DmaAllocator::lastError() const
{
    return m_lastError;
}

std::vector<std::string> DmaAllocator::heapCandidates() const
{
    std::vector<std::string> paths;
    if (!m_preferredHeapPath.empty()) {
        paths.push_back(m_preferredHeapPath);
    }

    DIR* dir = opendir(kDmaHeapDir);
    if (dir == nullptr) {
        return paths;
    }

    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        std::string path = std::string(kDmaHeapDir) + "/" + entry->d_name;
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
            paths.push_back(path);
        }
    }
    closedir(dir);

    if (paths.size() > 1) {
        std::sort(paths.begin() + (m_preferredHeapPath.empty() ? 0 : 1), paths.end());
    }

    return paths;
}

bool DmaAllocator::allocateFromHeap(const std::string& heapPath,
                                    size_t size,
                                    DmaMemory& out)
{
    int heapFd = open(heapPath.c_str(), O_RDWR | O_CLOEXEC);
    if (heapFd < 0) {
        setError(errnoText("打开 DMA heap 失败 " + heapPath));
        return false;
    }

    DmaHeapAllocationData alloc {};
    alloc.len = size;
    alloc.fd = 0;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    alloc.heap_flags = 0;

    if (ioctlRetry(heapFd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        setError(errnoText("DMA heap 分配失败 " + heapPath));
        close(heapFd);
        return false;
    }

    close(heapFd);

    void* va = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, alloc.fd, 0);
    if (va == MAP_FAILED) {
        setError(errnoText("mmap DMA buffer 失败"));
        close(static_cast<int>(alloc.fd));
        return false;
    }

    out = DmaMemory(static_cast<int>(alloc.fd), va, size);
    return true;
}

void DmaAllocator::setError(const std::string& message)
{
    m_lastError = message;
}
