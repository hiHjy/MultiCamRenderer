#include "DmaAllocator.hpp"

#include <cstdlib>
#include <iostream>
#include <utility>

namespace {

size_t parseSize(const char* text, size_t defaultValue)
{
    if (text == nullptr) {
        return defaultValue;
    }

    char* end = nullptr;
    unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || value == 0) {
        return defaultValue;
    }
    return static_cast<size_t>(value);
}

} // namespace

int main(int argc, char** argv)
{
    const size_t size = argc > 1 ? parseSize(argv[1], 460800) : 460800;

    DmaAllocator allocator;
    DmaMemory memory;
    if (!allocator.allocate(size, memory)) {
        std::cerr << "DMA 分配失败: " << allocator.lastError() << "\n";
        return 1;
    }

    std::cout << "分配成功: fd=" << memory.fd()
              << " va=" << memory.va()
              << " size=" << memory.size()
              << "\n";

    DmaMemory moved = std::move(memory);
    std::cout << "move 后: old_valid=" << memory.valid()
              << " new_fd=" << moved.fd()
              << " new_va=" << moved.va()
              << " new_size=" << moved.size()
              << "\n";

    moved.reset();
    std::cout << "手动释放后: valid=" << moved.valid() << "\n";
    return 0;
}
