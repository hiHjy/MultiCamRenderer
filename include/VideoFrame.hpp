#pragma once

#include "VideoTypes.hpp"

#include <cstddef>
#include <cstdint>

struct VideoFrame {
    int streamId = -1;

    int dmaFd = -1;
    void* va = nullptr;
    size_t capacity = 0;
    size_t bytesUsed = 0;

    int width = 0;
    int height = 0;
    int stride = 0;
    PixelFormat format = PixelFormat::Unknown;
    uint32_t nativeFormat = 0;

    uint64_t timestampUs = 0;
    uint64_t sequence = 0;

    int bufferIndex = -1;
};
