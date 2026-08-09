#pragma once

#include "VideoTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

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

// FrameLease 只负责“最后一个使用者释放后执行归还动作”。
// 对 V4L2 摄像头帧来说，release 通常不是直接 QBUF，
// 而是把 bufferIndex 投递回采集线程，由采集线程统一 QBUF。
class FrameLease {
public:
    explicit FrameLease(std::function<void()> release)
        : m_release(std::move(release))
    {
    }

    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;

    FrameLease(FrameLease&&) = delete;
    FrameLease& operator=(FrameLease&&) = delete;

    ~FrameLease()
    {
        if (m_release)
            m_release();
    }

private:
    std::function<void()> m_release;
};

// FramePacket 是跨层传递的一帧：frame 描述图像和 DMA 资源，
// lease 保护这块资源在 sink 使用期间不会被提前归还给生产者。
struct FramePacket {
    VideoFrame frame;
    std::shared_ptr<FrameLease> lease;
};
