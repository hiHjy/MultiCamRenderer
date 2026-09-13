#include "VideoFrame.hpp"

int videoFrameAlignUp(int value, int alignment)
{
    if (alignment <= 1)
        return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

int videoFrameBytesPerPixelForStride(PixelFormat format)
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

int videoFrameMinDimensionAlignment(PixelFormat format)
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

int videoFrameAlignedStride(PixelFormat format, int visibleWidth, int byteAlignment)
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

int videoFrameAlignedHeightStride(PixelFormat format, int visibleHeight, int dimensionAlignment)
{
    if (visibleHeight <= 0)
        return 0;
    const int alignment = dimensionAlignment > 0
        ? dimensionAlignment
        : videoFrameMinDimensionAlignment(format);
    return videoFrameAlignUp(visibleHeight, alignment);
}

int videoFrameEffectiveStride(const VideoFrame& frame)
{
    return frame.stride > 0 ? frame.stride : frame.width;
}

int videoFrameEffectiveHeightStride(const VideoFrame& frame)
{
    return frame.heightStride > 0 ? frame.heightStride : frame.height;
}

size_t videoFrameBufferSizeFor(PixelFormat format,
                               int widthStride,
                               int heightStride,
                               VideoBufferSizeMode mode)
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

size_t videoFrameBufferSizeFor(PixelFormat format,
                               int visibleWidth,
                               int visibleHeight,
                               int strideByteAlignment,
                               VideoBufferSizeMode mode)
{
    const int stride = videoFrameAlignedStride(format, visibleWidth, strideByteAlignment);
    const int heightStride = videoFrameAlignedHeightStride(format, visibleHeight);
    return videoFrameBufferSizeFor(format, stride, heightStride, mode);
}

size_t videoFrameBufferSize(const VideoFrame& frame, VideoBufferSizeMode mode)
{
    return videoFrameBufferSizeFor(frame.format,
                                   videoFrameEffectiveStride(frame),
                                   videoFrameEffectiveHeightStride(frame),
                                   mode);
}

size_t videoFramePlaneOffset(PixelFormat format, int widthStride, int heightStride, int plane)
{
    if (plane <= 0 || widthStride <= 0 || heightStride <= 0)
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

size_t videoFramePlaneOffset(const VideoFrame& frame, int plane)
{
    return videoFramePlaneOffset(frame.format,
                                 videoFrameEffectiveStride(frame),
                                 videoFrameEffectiveHeightStride(frame),
                                 plane);
}
