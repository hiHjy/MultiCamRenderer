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

    rga_buffer_t srcBuffer = wrapbuffer_fd_t(src.dmaFd,
                                            src.width,
                                            src.height,
                                            src.stride,
                                            src.height,
                                            rgaFormat);
    rga_buffer_t dstBuffer = wrapbuffer_fd_t(dst.dmaFd,
                                            dst.width,
                                            dst.height,
                                            dst.stride,
                                            dst.height,
                                            rgaFormat);

    IM_STATUS status = imcopy(srcBuffer, dstBuffer, 1);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        std::ostringstream oss;
        oss << "RGA copy 失败: " << imStrError(status);
        setError(oss.str());
        return false;
    }

    copyMeta(src, dst);
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

    rga_buffer_t srcBuffer = wrapbuffer_fd_t(src.dmaFd,
                                            src.width,
                                            src.height,
                                            src.stride,
                                            src.height,
                                            rgaFormat);
    rga_buffer_t dstBuffer = wrapbuffer_fd_t(dst.dmaFd,
                                            dst.width,
                                            dst.height,
                                            dst.stride,
                                            dst.height,
                                            rgaFormat);

    IM_STATUS status = imresize(srcBuffer, dstBuffer, 0, 0, IM_INTERP_DEFAULT, 1);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        std::ostringstream oss;
        oss << "RGA resize 失败: " << imStrError(status);
        setError(oss.str());
        return false;
    }

    dst.streamId = src.streamId;
    dst.bytesUsed = dst.capacity;
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

    if (frame.width <= 0 || frame.height <= 0 || frame.stride <= 0) {
        setError(std::string(name) + " 宽高或 stride 无效");
        return false;
    }

    if (frame.capacity == 0) {
        setError(std::string(name) + " capacity 为 0");
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

void RgaEngine::copyMeta(const VideoFrame& src, VideoFrame& dst)
{
    dst.streamId = src.streamId;
    dst.bytesUsed = src.bytesUsed;
    dst.nativeFormat = src.nativeFormat;
    dst.timestampUs = src.timestampUs;
    dst.sequence = src.sequence;
}

void RgaEngine::setError(const std::string& message)
{
    m_lastError = message;
}
