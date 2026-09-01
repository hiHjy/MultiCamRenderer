#pragma once

#include "VideoTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

enum class VideoBufferSizeMode {
    // 只计算图像真实 payload。NV12/YUV420P 按 1.5 bytes/pixel。
    Payload,
    // RK MPP 解码器使用外部输出 buffer 时，NV12/YUV420P 预留到 stride * heightStride * 2。
    MppDecoderOutput,
};

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

// 把 value 向上对齐到 alignment 的整数倍。
//
// 参数：
// - value: 要对齐的原始数值，例如宽度、行数、字节数。
// - alignment: 对齐粒度。<= 1 表示不需要对齐，直接返回 value。
//
// 返回：
// - 对齐后的数值。比如 value=641, alignment=16，返回 656。
inline int videoFrameAlignUp(int value, int alignment)
{
    if (alignment <= 1)
        return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

// 返回当前格式计算“每行 pitch”时使用的单像素字节数。
//
// 参数：
// - format: 像素格式。
//
// 返回：
// - NV12/YUV420P 返回 1，因为它们的 Y plane 一像素占 1 字节，stride 按 Y plane 计算。
// - YUYV 返回 2。
// - RGBA8888 返回 4。
// - MJPEG/Unknown/Auto 返回 0，因为压缩格式或未知格式不能用固定 bpp 描述裸帧 pitch。
inline int videoFrameBytesPerPixelForStride(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12:
    case PixelFormat::YUV420P:
        return 1;
    case PixelFormat::YUYV:
        return 2;
    case PixelFormat::RGBA8888:
        return 4;
    case PixelFormat::MJPEG:
    case PixelFormat::Unknown:
    case PixelFormat::Auto:
        return 0;
    }
    return 0;
}

// 返回指定格式最小的宽高对齐粒度。
//
// 参数：
// - format: 像素格式。
//
// 返回：
// - YUV 类格式返回 2，因为 4:2:0 / 4:2:2 通常要求偶数宽高。
// - RGBA8888 和压缩/未知格式返回 1。
//
// 注意：
// - 这是通用最小对齐，不等于某个硬件模块的额外要求。
// - MPP/JPEGD 外部输出如果要求 16 对齐，应在调用处显式传 16。
inline int videoFrameMinDimensionAlignment(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12:
    case PixelFormat::YUV420P:
    case PixelFormat::YUYV:
        return 2;
    case PixelFormat::RGBA8888:
    case PixelFormat::MJPEG:
    case PixelFormat::Unknown:
    case PixelFormat::Auto:
        return 1;
    }
    return 1;
}

// 计算某个可见宽度对应的横向 stride，单位是“像素”。
//
// 参数：
// - format: 像素格式，用来决定 bpp 和最小宽度对齐。
// - visibleWidth: 图像真实可见宽度，单位是像素。
// - byteAlignment: 行 pitch 的字节对齐要求。0 表示不做额外字节对齐。
//
// 返回：
// - 横向 stride，单位仍然是像素，不是字节。
//
// 示例：
// - RGBA8888, visibleWidth=641, byteAlignment=64：
//   先按 4 字节/像素算 pitch，再把 pitch 对齐到 64 字节，最后换回像素 stride。
inline int videoFrameAlignedStride(PixelFormat format, int visibleWidth, int byteAlignment = 0)
{
    if (visibleWidth <= 0)
        return 0;

    const int bpp = videoFrameBytesPerPixelForStride(format);
    const int dimensionAlignment = videoFrameMinDimensionAlignment(format);
    int stride = videoFrameAlignUp(visibleWidth, dimensionAlignment);

    if (bpp > 0 && byteAlignment > 0) {
        const int alignedBytes = videoFrameAlignUp(stride * bpp, byteAlignment);
        stride = videoFrameAlignUp((alignedBytes + bpp - 1) / bpp, dimensionAlignment);
    }

    return stride;
}

// 计算纵向 stride，也就是 buffer 实际预留的行数。
//
// 参数：
// - format: 像素格式，用来决定默认最小行对齐。
// - visibleHeight: 图像真实可见高度，单位是行/像素。
// - dimensionAlignment: 额外指定的行数对齐。0 表示使用 format 的默认最小对齐。
//
// 返回：
// - 纵向 stride，单位是行。
//
// 示例：
// - NV12, visibleHeight=481, dimensionAlignment=16，返回 496。
inline int videoFrameAlignedHeightStride(PixelFormat format,
                                         int visibleHeight,
                                         int dimensionAlignment = 0)
{
    if (visibleHeight <= 0)
        return 0;
    const int alignment = dimensionAlignment > 0
        ? dimensionAlignment
        : videoFrameMinDimensionAlignment(format);
    return videoFrameAlignUp(visibleHeight, alignment);
}

// 取一帧的有效横向 stride。
//
// 参数：
// - frame: 视频帧描述。
//
// 返回：
// - frame.stride > 0 时返回 frame.stride。
// - 否则返回 frame.width，表示紧密排布兜底。
//
// 注意：
// - 返回单位是像素，不是字节。
inline int videoFrameEffectiveStride(const VideoFrame& frame)
{
    return frame.stride > 0 ? frame.stride : frame.width;
}

// 取一帧的有效纵向 stride。
//
// 参数：
// - frame: 视频帧描述。
//
// 返回：
// - frame.heightStride > 0 时返回 frame.heightStride。
// - 否则返回 frame.height，表示按可见高度兜底。
inline int videoFrameEffectiveHeightStride(const VideoFrame& frame)
{
    return frame.heightStride > 0 ? frame.heightStride : frame.height;
}

// 按已经确定的 stride 计算 buffer 大小。
//
// 参数：
// - format: 像素格式。
// - widthStride: 横向 stride，单位是像素，不是字节。
// - heightStride: 纵向 stride，单位是行。
// - mode: 计算模式。
//   - Payload: 只计算图像真实 payload。
//   - MppDecoderOutput: RK MPP 外部解码输出 buffer 预留模式。
//
// 返回：
// - 所需 buffer 字节数。
// - MJPEG/Unknown/Auto 返回 0，因为它们不能用裸帧 layout 计算固定大小。
//
// 注意：
// - NV12/YUV420P 在 Payload 模式下是 stride * heightStride * 3 / 2。
// - NV12/YUV420P 在 MppDecoderOutput 模式下是 stride * heightStride * 2。
inline size_t videoFrameBufferSizeFor(PixelFormat format,
                                      int widthStride,
                                      int heightStride,
                                      VideoBufferSizeMode mode = VideoBufferSizeMode::Payload)
{
    if (widthStride <= 0 || heightStride <= 0)
        return 0;

    const size_t pixels = static_cast<size_t>(widthStride) * static_cast<size_t>(heightStride);
    switch (format) {
    case PixelFormat::NV12:
    case PixelFormat::YUV420P:
        if (mode == VideoBufferSizeMode::MppDecoderOutput)
            return pixels * 2;
        return pixels * 3 / 2;
    case PixelFormat::YUYV:
        return pixels * 2;
    case PixelFormat::RGBA8888:
        return pixels * 4;
    case PixelFormat::MJPEG:
    case PixelFormat::Unknown:
    case PixelFormat::Auto:
        return 0;
    }
    return 0;
}

// 按可见宽高和行 pitch 对齐要求计算 buffer 大小。
//
// 参数：
// - format: 像素格式。
// - visibleWidth: 图像真实可见宽度，单位是像素。
// - visibleHeight: 图像真实可见高度，单位是像素/行。
// - strideByteAlignment: 行 pitch 的字节对齐要求。0 表示不做额外字节对齐。
// - mode: buffer 大小计算模式，含义同上一个重载。
//
// 返回：
// - 先计算横向 stride 和纵向 stride，再返回对应 buffer 字节数。
inline size_t videoFrameBufferSizeFor(PixelFormat format,
                                      int visibleWidth,
                                      int visibleHeight,
                                      int strideByteAlignment,
                                      VideoBufferSizeMode mode)
{
    const int stride = videoFrameAlignedStride(format, visibleWidth, strideByteAlignment);
    const int heightStride = videoFrameAlignedHeightStride(format, visibleHeight);
    return videoFrameBufferSizeFor(format, stride, heightStride, mode);
}

// 按 VideoFrame 当前 layout 计算 buffer 大小。
//
// 参数：
// - frame: 视频帧描述。使用 frame.format、有效 stride、有效 heightStride。
// - mode: buffer 大小计算模式。
//
// 返回：
// - 当前 frame layout 对应的 buffer 字节数。
//
// 注意：
// - 这个函数不看 frame.capacity，也不看 frame.bytesUsed。
// - capacity 是底层实际分配了多大；bytesUsed 是当前生产者写了多少有效数据。
// - 本函数只回答“按这个 layout 理论上需要多少字节”。
inline size_t videoFrameBufferSize(const VideoFrame& frame,
                                   VideoBufferSizeMode mode = VideoBufferSizeMode::Payload)
{
    return videoFrameBufferSizeFor(frame.format,
                                   videoFrameEffectiveStride(frame),
                                   videoFrameEffectiveHeightStride(frame),
                                   mode);
}

// 计算指定 plane 在 buffer 内的字节偏移。
//
// 参数：
// - format: 像素格式。
// - widthStride: 横向 stride，单位是像素，不是字节。
// - heightStride: 纵向 stride，单位是行。
// - plane: plane 下标，从 0 开始。0 表示 Y/packed 起始 plane。
//
// 返回：
// - plane 的字节偏移。
// - 不存在的 plane 返回 0。
//
// 当前支持：
// - NV12: plane 0 是 Y，plane 1 是交织 UV，offset = stride * heightStride。
// - YUV420P: plane 0 是 Y，plane 1 是 U，plane 2 是 V。
// - YUYV/RGBA8888 是 packed 格式，只有 plane 0。
inline size_t videoFramePlaneOffset(PixelFormat format,
                                    int widthStride,
                                    int heightStride,
                                    int plane)
{
    if (plane <= 0)
        return 0;
    if (widthStride <= 0 || heightStride <= 0)
        return 0;

    const size_t ySize = static_cast<size_t>(widthStride) * static_cast<size_t>(heightStride);
    switch (format) {
    case PixelFormat::NV12:
        return plane == 1 ? ySize : 0;
    case PixelFormat::YUV420P:
        if (plane == 1)
            return ySize;
        if (plane == 2)
            return ySize + ySize / 4;
        return 0;
    case PixelFormat::YUYV:
    case PixelFormat::RGBA8888:
    case PixelFormat::MJPEG:
    case PixelFormat::Unknown:
    case PixelFormat::Auto:
        return 0;
    }
    return 0;
}

// 按 VideoFrame 当前 layout 计算指定 plane 的字节偏移。
//
// 参数：
// - frame: 视频帧描述。使用 frame.format、有效 stride、有效 heightStride。
// - plane: plane 下标，从 0 开始。
//
// 返回：
// - plane 的字节偏移。
inline size_t videoFramePlaneOffset(const VideoFrame& frame, int plane)
{
    return videoFramePlaneOffset(frame.format,
                                 videoFrameEffectiveStride(frame),
                                 videoFrameEffectiveHeightStride(frame),
                                 plane);
}
