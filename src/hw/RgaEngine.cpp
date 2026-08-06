#include "hw/RgaEngine.hpp"

#include "im2d.h"
#include "rga.h"

#include <sstream>

bool RgaEngine::copy(const VideoFrame& src, VideoFrame& dst)
{
    if (!validateFrame(src, "src") || !validateFrame(dst, "dst") ||
        !validateSameFormat(src, dst)) {
        return false;
    }

    if (src.width != dst.width || src.height != dst.height) {
        setError("RGA copy 要求源和目标宽高一致，缩放请调用 resize");
        return false;
    }

    const int rgaFormat = toRgaFormat(src.format);
    if (rgaFormat < 0) {
        return false;
    }

    // RGA 会按最终传入的 stride/hstride 访问内存。
    // 所以在 wrapbuffer 之前先按同一套规则检查 capacity，避免硬件越界读写。
    if (!validateCapacity(src, "src") || !validateCapacity(dst, "dst")) {
        return false;
    }

    rga_buffer_t srcBuffer = wrapbuffer_fd_t(src.dmaFd,
                                            src.width,
                                            src.height,
                                            effectiveStride(src),
                                            effectiveHeightStride(src),
                                            rgaFormat);
    rga_buffer_t dstBuffer = wrapbuffer_fd_t(dst.dmaFd,
                                            dst.width,
                                            dst.height,
                                            effectiveStride(dst),
                                            effectiveHeightStride(dst),
                                            rgaFormat);

    IM_STATUS status = imcopy(srcBuffer, dstBuffer, 1);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        std::ostringstream oss;
        oss << "RGA copy 失败: " << imStrError(status);
        setError(oss.str());
        return false;
    }

    copyMeta(src, dst);
    dst.bytesUsed = requiredSize(dst);
    m_lastError.clear();
    return true;
}

bool RgaEngine::resize(const VideoFrame& src, VideoFrame& dst)
{
    if (!validateFrame(src, "src") || !validateFrame(dst, "dst") ||
        !validateSameFormat(src, dst)) {
        return false;
    }

    const int rgaFormat = toRgaFormat(src.format);
    if (rgaFormat < 0) {
        return false;
    }

    // resize 的 dst 通常来自消费者自己的 DmaBufferPool。
    // Pool 只保证内存大小，真实 layout 要由调用者填在 dst 里。
    if (!validateCapacity(src, "src") || !validateCapacity(dst, "dst")) {
        return false;
    }

    rga_buffer_t srcBuffer = wrapbuffer_fd_t(src.dmaFd,
                                            src.width,
                                            src.height,
                                            effectiveStride(src),
                                            effectiveHeightStride(src),
                                            rgaFormat);
    rga_buffer_t dstBuffer = wrapbuffer_fd_t(dst.dmaFd,
                                            dst.width,
                                            dst.height,
                                            effectiveStride(dst),
                                            effectiveHeightStride(dst),
                                            rgaFormat);

    IM_STATUS status = imresize(srcBuffer, dstBuffer, 0, 0, IM_INTERP_DEFAULT, 1);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        std::ostringstream oss;
        oss << "RGA resize 失败: " << imStrError(status);
        setError(oss.str());
        return false;
    }

    dst.streamId = src.streamId;
    dst.bytesUsed = requiredSize(dst);
    dst.nativeFormat = src.nativeFormat;
    dst.timestampUs = src.timestampUs;
    dst.sequence = src.sequence;
    m_lastError.clear();
    return true;
}

const std::string& RgaEngine::lastError() const
{
    return m_lastError;
}

int RgaEngine::toRgaFormat(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12:
        return RK_FORMAT_YCbCr_420_SP;
    case PixelFormat::YUV420P:
        return RK_FORMAT_YCbCr_420_P;
    case PixelFormat::YUYV:
        return RK_FORMAT_YUYV_422;
    case PixelFormat::Unknown:
    case PixelFormat::Auto:
        setError("RGA 不支持 Unknown/Auto PixelFormat，必须传入已协商格式");
        return -1;
    }

    setError("未知 PixelFormat");
    return -1;
}

bool RgaEngine::validateFrame(const VideoFrame& frame, const char* name)
{
    if (frame.dmaFd < 0) {
        setError(std::string(name) + " dmaFd 无效");
        return false;
    }

    if (frame.width <= 0 || frame.height <= 0) {
        setError(std::string(name) + " 宽高无效");
        return false;
    }

    if (frame.capacity == 0) {
        setError(std::string(name) + " capacity 为 0");
        return false;
    }

    return true;
}

bool RgaEngine::validateCapacity(const VideoFrame& frame, const char* name)
{
    const size_t size = requiredSize(frame);
    if (size == 0) {
        return false;
    }

    if (size > frame.capacity) {
        std::ostringstream oss;
        oss << name << " buffer 太小: required=" << size
            << " capacity=" << frame.capacity
            << " visible=" << frame.width << "x" << frame.height
            << " stride=" << effectiveStride(frame) << "x" << effectiveHeightStride(frame);
        setError(oss.str());
        return false;
    }

    return true;
}

bool RgaEngine::validateSameFormat(const VideoFrame& src, const VideoFrame& dst)
{
    if (src.format != dst.format) {
        setError("当前 RgaEngine 只支持相同 PixelFormat 的 copy/resize");
        return false;
    }

    return true;
}

int RgaEngine::effectiveStride(const VideoFrame& frame) const
{
    // 上层没有显式指定 stride 时，按紧密排布处理。
    // 如果要 64 字节等硬件对齐，调用 RGA 前手动填 frame.stride。
    return frame.stride > 0 ? frame.stride : frame.width;
}

int RgaEngine::effectiveHeightStride(const VideoFrame& frame) const
{
    // V4L2 当前没有可靠的纵向 stride 就保持 0。
    // RGA 包装时把 0 当作 visible height；MPP 解码输出有 ver_stride 时再填真实值。
    return frame.heightStride > 0 ? frame.heightStride : frame.height;
}

size_t RgaEngine::requiredSize(const VideoFrame& frame)
{
    const int widthStride = effectiveStride(frame);
    const int heightStride = effectiveHeightStride(frame);
    if (widthStride <= 0 || heightStride <= 0) {
        setError("RGA stride 无效");
        return 0;
    }

    switch (frame.format) {
    case PixelFormat::NV12:
    case PixelFormat::YUV420P:
        // NV12/YUV420P 都按 1.5 bytes/pixel 估算底层容量，
        // 这里使用 stride 后的布局尺寸，不使用 visible width/height。
        return static_cast<size_t>(widthStride) * static_cast<size_t>(heightStride) * 3 / 2;
    case PixelFormat::YUYV:
        return static_cast<size_t>(widthStride) * static_cast<size_t>(heightStride) * 2;
    case PixelFormat::Unknown:
    case PixelFormat::Auto:
        setError("RGA 不能计算 Unknown/Auto PixelFormat 的 buffer size");
        return 0;
    }

    setError("未知 PixelFormat，无法计算 RGA buffer size");
    return 0;
}

void RgaEngine::copyMeta(const VideoFrame& src, VideoFrame& dst)
{
    dst.streamId = src.streamId;
    dst.nativeFormat = src.nativeFormat;
    dst.timestampUs = src.timestampUs;
    dst.sequence = src.sequence;
}

void RgaEngine::setError(const std::string& message)
{
    m_lastError = message;
}
