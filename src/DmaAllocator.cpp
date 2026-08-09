#include "DmaAllocator.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <drm/drm.h>
#include <drm/drm_mode.h>
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
constexpr const char* kDriDir = "/dev/dri";
constexpr size_t kDrmDumbRowBytes = 4096;

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

bool startsWith(const char* text, const char* prefix)
{
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

size_t alignUpSize(size_t value, size_t alignment)
{
    if (alignment <= 1)
        return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

} // namespace

DmaMemory::DmaMemory(int fd, void* va, size_t size)
    : m_fd(fd)
    , m_va(va)
    , m_size(size)
{
}

DmaMemory::DmaMemory(int fd, void* va, size_t size, int drmFd, uint32_t drmHandle)
    : m_fd(fd)
    , m_va(va)
    , m_size(size)
    , m_drmFd(drmFd)
    , m_drmHandle(drmHandle)
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
    , m_drmFd(other.m_drmFd)
    , m_drmHandle(other.m_drmHandle)
{
    other.m_fd = -1;
    other.m_va = nullptr;
    other.m_size = 0;
    other.m_drmFd = -1;
    other.m_drmHandle = 0;
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
    m_drmFd = other.m_drmFd;
    m_drmHandle = other.m_drmHandle;

    other.m_fd = -1;
    other.m_va = nullptr;
    other.m_size = 0;
    other.m_drmFd = -1;
    other.m_drmHandle = 0;
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

    if (m_drmFd >= 0 && m_drmHandle != 0) {
        drm_mode_destroy_dumb destroy {};
        destroy.handle = m_drmHandle;
        (void)ioctlRetry(m_drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }

    if (m_drmFd >= 0) {
        close(m_drmFd);
    }

    m_fd = -1;
    m_va = nullptr;
    m_size = 0;
    m_drmFd = -1;
    m_drmHandle = 0;
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

    std::string lastAllocError;
    const std::vector<std::string> heaps = heapCandidates();
    for (const std::string& heap : heaps) {
        if (allocateFromHeap(heap, size, out)) {
            m_lastError.clear();
            return true;
        }
        lastAllocError = m_lastError;
    }

    const std::vector<std::string> drmNodes = drmCandidates();
    for (const std::string& drmNode : drmNodes) {
        if (allocateFromDrm(drmNode, size, out)) {
            m_lastError.clear();
            return true;
        }
        lastAllocError = m_lastError;
    }

    setError(lastAllocError.empty()
                 ? "DMA 分配失败: 未找到可用 dma_heap 或 DRM dumb buffer 节点"
                 : lastAllocError);
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

std::vector<std::string> DmaAllocator::drmCandidates() const
{
    std::vector<std::string> cards;
    std::vector<std::string> renders;

    DIR* dir = opendir(kDriDir);
    if (dir == nullptr) {
        return {};
    }

    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        std::string path = std::string(kDriDir) + "/" + entry->d_name;
        if (startsWith(entry->d_name, "card")) {
            cards.push_back(path);
        } else if (startsWith(entry->d_name, "renderD")) {
            renders.push_back(path);
        }
    }
    closedir(dir);

    std::sort(cards.begin(), cards.end());
    std::sort(renders.begin(), renders.end());

    cards.insert(cards.end(), renders.begin(), renders.end());
    return cards;
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

bool DmaAllocator::allocateFromDrm(const std::string& drmPath,
                                   size_t size,
                                   DmaMemory& out)
{
    if (size > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) *
                   kDrmDumbRowBytes) {
        setError("DRM dumb buffer 请求大小过大");
        return false;
    }

    int drmFd = open(drmPath.c_str(), O_RDWR | O_CLOEXEC);
    if (drmFd < 0) {
        setError(errnoText("打开 DRM 节点失败 " + drmPath));
        return false;
    }

    const size_t rows = alignUpSize(size, kDrmDumbRowBytes) / kDrmDumbRowBytes;
    drm_mode_create_dumb create {};
    create.width = static_cast<uint32_t>(kDrmDumbRowBytes);
    create.height = static_cast<uint32_t>(rows);
    create.bpp = 8;

    if (ioctlRetry(drmFd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        setError(errnoText("DRM dumb buffer 分配失败 " + drmPath));
        close(drmFd);
        return false;
    }

    drm_mode_map_dumb map {};
    map.handle = create.handle;
    if (ioctlRetry(drmFd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        setError(errnoText("DRM dumb buffer MAP_DUMB 失败 " + drmPath));
        drm_mode_destroy_dumb destroy {};
        destroy.handle = create.handle;
        (void)ioctlRetry(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        close(drmFd);
        return false;
    }

    void* va = mmap(nullptr,
                    static_cast<size_t>(create.size),
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED,
                    drmFd,
                    static_cast<off_t>(map.offset));
    if (va == MAP_FAILED) {
        setError(errnoText("mmap DRM dumb buffer 失败 " + drmPath));
        drm_mode_destroy_dumb destroy {};
        destroy.handle = create.handle;
        (void)ioctlRetry(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        close(drmFd);
        return false;
    }

    drm_prime_handle prime {};
    prime.handle = create.handle;
    prime.flags = DRM_CLOEXEC | DRM_RDWR;
    prime.fd = -1;
    if (ioctlRetry(drmFd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
        setError(errnoText("DRM dumb buffer 导出 PRIME fd 失败 " + drmPath));
        munmap(va, static_cast<size_t>(create.size));
        drm_mode_destroy_dumb destroy {};
        destroy.handle = create.handle;
        (void)ioctlRetry(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        close(drmFd);
        return false;
    }

    out = DmaMemory(prime.fd,
                    va,
                    static_cast<size_t>(create.size),
                    drmFd,
                    create.handle);
    return true;
}

void DmaAllocator::setError(const std::string& message)
{
    m_lastError = message;
}
