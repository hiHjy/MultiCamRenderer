#pragma once

#include "VideoTypes.hpp"

#include <cstddef>
#include <cstdint>

struct VideoFrame {
    int streamId = -1;

    // 这几个字段描述底层 DMA 资源。VideoFrame 本身不拥有 fd/va，
    // 它只是某个 Source/Pool/Decoder 持有资源时暴露出来的视图。
    int dmaFd = -1;
    void* va = nullptr;
    size_t capacity = 0;
    size_t bytesUsed = 0;

    // 这几个字段描述“当前这块 buffer 被解释成什么图像”。
    // DmaBufferPool 不会提前填写它们；谁往 buffer 里生产图像，
    // 谁负责填真实的 width/height/format/stride。
    int width = 0;
    int height = 0;
    // 横向 stride，按像素计。RGA/MPP 通常称作 hor_stride。
    // 为 0 时表示使用方按 width 作为默认值。
    int stride = 0;
    // 纵向 stride，按行计。MPP 通常称作 ver_stride。
    // 对 NV12 来说，UV plane 常见偏移是 stride * heightStride。
    // V4L2 当前没给这个值时保持 0，使用方按 height 作为默认值。
    int heightStride = 0;
    PixelFormat format = PixelFormat::Unknown;
    uint32_t nativeFormat = 0;

    uint64_t timestampUs = 0;
    uint64_t sequence = 0;

    int bufferIndex = -1;
};
