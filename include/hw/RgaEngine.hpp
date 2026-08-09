#pragma once

#include "VideoFrame.hpp"

#include <cstddef>
#include <string>

enum class RgaOp {
    Auto,
    Copy,
    Resize,
    ConvertColor,
};

enum class RgaRotation {
    Rotate0,
    Rotate90,
    Rotate180,
    Rotate270,
};

enum class RgaMirror {
    None,
    Horizontal,
    Vertical,
    Both,
};

struct RgaRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct RgaOperation {
    RgaOp op = RgaOp::Auto;
    RgaRotation rotation = RgaRotation::Rotate0;
    RgaMirror mirror = RgaMirror::None;
    RgaRect crop {};

    // 本次 RGA 操作使用的输出行 pitch 对齐，单位是字节。
    // 0 表示关闭自动对齐；默认 16 字节是 RGA 路径里比较保守的运行布局。
    // 注意：这个只影响 dst 的自动 stride；显式传入的 dst.stride 会被保留并校验。
    int dstStrideByteAlignment = 16;
};

struct RgaGeometry {
    int width = 0;
    int height = 0;
};

// RgaEngine 是 VideoFrame -> VideoFrame 的 RGA 统一入口。
//
// 使用约定：
// 1. src 的 width/height/stride/heightStride 必须来自真实生产者，比如 V4L2/MPP。
//    RgaEngine 不会改 src 的 layout，因为源 buffer 的布局不是我们能猜的。
// 2. dst 必须先提供 dmaFd/capacity/format；width/height/stride 可以留 0。
//    RgaEngine 会根据 crop/rotate/resize 自动推导 dst 的真实可见尺寸和 stride。
// 3. VideoFrame::stride 的单位是“像素”，不是字节。
//    例如 RGBA8888 的 pitchBytes = stride * 4。
// 4. RGA 操作默认输出行 pitch 按 16 字节对齐；pool 申请容量建议按 64 字节 pitch 预留。
//    这样 buffer capacity 大于等于 RGA 实际访问量，后续切到更严格布局也不容易溢出。
// 5. visible width/height 只表示真实图像区域；stride/heightStride 表示底层 buffer 布局。
//    显示或导出 DMA-BUF 时必须把 pitch 一起传出去，不能只看 visible width。
class RgaEngine {
public:
    RgaEngine() = default;

    bool rga(const VideoFrame& src, VideoFrame& dst, const RgaOperation& op = {});
    bool copy(const VideoFrame& src, VideoFrame& dst);
    bool resize(const VideoFrame& src, VideoFrame& dst);

    // 按图像 layout 计算 DMA-BUF 容量。默认 64 字节 pitch 对齐用于 pool 预留容量，
    // 目的是让 pool capacity 大于等于 RGA 实际访问量，而不是强迫 RGA 每次都用 64 字节 pitch。
    static size_t bufferSizeFor(PixelFormat format,
                                int width,
                                int height,
                                int strideByteAlignment = 64);

    const std::string& lastError() const;

private:
    int toRgaFormat(PixelFormat format);
    bool validateFrame(const VideoFrame& frame, const char* name);
    bool validateDmaFrame(const VideoFrame& frame, const char* name);
    bool validateSameFormat(const VideoFrame& src, const VideoFrame& dst);
    bool validateCapacity(const VideoFrame& frame, const char* name);
    bool validateWindowAlignment(const VideoFrame& frame,
                                 const RgaRect& rect,
                                 const char* name);
    bool computeOutputGeometry(const VideoFrame& src,
                               const VideoFrame& dst,
                               const RgaOperation& op,
                               RgaGeometry& geometry);
    void applyOutputGeometry(VideoFrame& dst, const RgaGeometry& geometry);
    bool normalizeDstLayout(VideoFrame& dst, const RgaOperation& op);
    bool validateStrideAlignment(const VideoFrame& frame,
                                 const RgaOperation& op,
                                 const char* name);
    int bytesPerPixel(PixelFormat format) const;
    int minDimensionAlignment(PixelFormat format) const;
    int alignedStridePixels(PixelFormat format,
                            int visibleWidth,
                            int byteAlignment) const;
    int transformUsage(const RgaOperation& op) const;
    int effectiveStride(const VideoFrame& frame) const;
    int effectiveHeightStride(const VideoFrame& frame) const;
    size_t requiredSize(const VideoFrame& frame);
    void fillMeta(const VideoFrame& src, VideoFrame& dst);
    void copyMeta(const VideoFrame& src, VideoFrame& dst);
    void setError(const std::string& message);

private:
    std::string m_lastError;
};
