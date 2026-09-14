#include "RgaEngine.hpp"

#include "im2d.h"
#include "rga.h"

#include <cstring>
#include <sstream>

bool RgaEngine::rga(const VideoFrame& src, VideoFrame& dst, const RgaOperation& op)
{
    if (!validateFrame(src, "src") || !validateDmaFrame(dst, "dst")) {
        return false;
    }

    if (op.op == RgaOp::Copy && !validateSameFormat(src, dst)) {
        return false;
    }

    RgaGeometry output {};
    if (!computeOutputGeometry(src, dst, op, output)) {
        return false;
    }

    // dst 的 fd/capacity/format 是资源事实；width/height/stride 是“本次输出 layout”。
    // crop/rotate 会改变真实输出尺寸，所以这里统一修正 dst 的可见尺寸和 stride。
    applyOutputGeometry(dst, output);
    if (!normalizeDstLayout(dst, op)) {
        return false;
    }

    const bool hasCrop = op.crop.width > 0 && op.crop.height > 0;
    if (hasCrop &&
        (op.crop.x < 0 || op.crop.y < 0 ||
         op.crop.x + op.crop.width > src.width ||
         op.crop.y + op.crop.height > src.height)) {
        std::ostringstream oss;
        oss << "RGA crop 越界: crop=[" << op.crop.x << "," << op.crop.y
            << "," << op.crop.width << "," << op.crop.height << "] src="
            << src.width << "x" << src.height;
        setError(oss.str());
        return false;
    }

    const int srcVisibleWidth = hasCrop ? op.crop.width : src.width;
    const int srcVisibleHeight = hasCrop ? op.crop.height : src.height;
    const RgaRect srcWindow {
        hasCrop ? op.crop.x : 0,
        hasCrop ? op.crop.y : 0,
        srcVisibleWidth,
        srcVisibleHeight,
    };

    if (!validateWindowAlignment(src, srcWindow, "src window")) {
        return false;
    }

    if (!validateCapacity(src, "src") || !validateCapacity(dst, "dst")) {
        return false;
    }

    if (!validateStrideAlignment(dst, op, "dst")) {
        return false;
    }

    const int srcFormat = toRgaFormat(src.format);
    const int dstFormat = toRgaFormat(dst.format);
    if (srcFormat < 0 || dstFormat < 0) {
        return false;
    }

    rga_buffer_t srcBuffer = wrapbuffer_fd_t(src.dmaFd,
                                            src.width,
                                            src.height,
                                            effectiveStride(src),
                                            effectiveHeightStride(src),
                                            srcFormat);
    rga_buffer_t dstBuffer = wrapbuffer_fd_t(dst.dmaFd,
                                            dst.width,
                                            dst.height,
                                            effectiveStride(dst),
                                            effectiveHeightStride(dst),
                                            dstFormat);

    // imcvtcolor 的语义比 improcess 更直接：它只做像素格式转换，不暗含 crop、
    // 旋转或缩放。把这条常用路径放在这里，OSD/demo 就不用散落原生 RGA 调用。
    if (op.op == RgaOp::ConvertColor) {
        if (hasCrop || op.rotation != RgaRotation::Rotate0 || op.mirror != RgaMirror::None ||
            src.width != dst.width || src.height != dst.height) {
            setError("RGA 色彩转换不支持 crop/旋转/镜像/缩放");
            return false;
        }
        const IM_STATUS check = imcheck(srcBuffer, dstBuffer, {}, {});
        if (check != IM_STATUS_SUCCESS && check != IM_STATUS_NOERROR) {
            setError(std::string("RGA 色彩转换 imcheck 失败: ") + imStrError(check));
            return false;
        }
        const IM_STATUS status = imcvtcolor(srcBuffer, dstBuffer, srcFormat, dstFormat);
        if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
            setError(std::string("RGA 色彩转换失败: ") + imStrError(status));
            return false;
        }
        fillMeta(src, dst);
        dst.bytesUsed = requiredSize(dst);
        m_lastError.clear();
        return true;
    }

    im_rect srcRect {
        srcWindow.x,
        srcWindow.y,
        srcVisibleWidth,
        srcVisibleHeight,
    };
    im_rect dstRect {
        0,
        0,
        dst.width,
        dst.height,
    };
    im_rect patRect {
        0,
        0,
        0,
        0,
    };
    rga_buffer_t patBuffer {};

    const int usage = IM_SYNC | transformUsage(op);
    IM_STATUS check = imcheck(srcBuffer, dstBuffer, srcRect, dstRect, usage);
    if (check != IM_STATUS_SUCCESS && check != IM_STATUS_NOERROR) {
        std::ostringstream oss;
        oss << "RGA imcheck 失败: " << imStrError(check);
        setError(oss.str());
        return false;
    }

    IM_STATUS status = improcess(srcBuffer,
                                 dstBuffer,
                                 patBuffer,
                                 srcRect,
                                 dstRect,
                                 patRect,
                                 0,
                                 nullptr,
                                 nullptr,
                                 usage);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        std::ostringstream oss;
        oss << "RGA improcess 失败: " << imStrError(status);
        setError(oss.str());
        return false;
    }

    fillMeta(src, dst);
    dst.bytesUsed = requiredSize(dst);
    m_lastError.clear();
    return true;
}

bool RgaEngine::copy(const VideoFrame& src, VideoFrame& dst)
{
    return rga(src, dst, RgaOperation {RgaOp::Copy});
}

bool RgaEngine::resize(const VideoFrame& src, VideoFrame& dst)
{
    return rga(src, dst, RgaOperation {RgaOp::Resize});
}

bool RgaEngine::convertColor(const VideoFrame& src, VideoFrame& dst)
{
    return rga(src, dst, RgaOperation {RgaOp::ConvertColor});
}

bool RgaEngine::composite(const VideoFrame& source,
                          const VideoFrame& overlay,
                          VideoFrame& destination,
                          const RgaOperation& op)
{
    // OSD 第一版只走最明确、已在 RV1126B SDK alpha_yuv_demo 验证过的路径：
    // NV12 视频源 + 预乘 alpha RGBA 小 overlay -> 独立 NV12 输出。
    if (source.format != PixelFormat::NV12 || destination.format != PixelFormat::NV12 ||
        overlay.format != PixelFormat::RGBA8888) {
        setError("RGA OSD 合成仅支持 NV12 视频 + RGBA8888 overlay -> NV12 输出");
        return false;
    }
    if (!validateFrame(source, "composite source") ||
        !validateFrame(overlay, "composite overlay") ||
        !validateFrame(destination, "composite destination")) {
        return false;
    }
    if (source.dmaFd == destination.dmaFd) {
        setError("RGA OSD 合成要求 source 与 destination 使用不同 DMA-BUF");
        return false;
    }

    const RgaRect sourceRect = op.crop;
    const RgaRect overlayRect = op.overlayCrop;
    const RgaRect destinationRect = op.compositeDestination;
    if (sourceRect.width <= 0 || sourceRect.height <= 0 ||
        overlayRect.width <= 0 || overlayRect.height <= 0 ||
        destinationRect.width <= 0 || destinationRect.height <= 0) {
        setError("RGA OSD 合成需要 source/overlay/destination 三个非空矩形");
        return false;
    }
    if (sourceRect.width != overlayRect.width || sourceRect.height != overlayRect.height ||
        sourceRect.width != destinationRect.width || sourceRect.height != destinationRect.height) {
        setError("RGA OSD 合成的 source/overlay/destination 矩形宽高必须一致");
        return false;
    }
    const auto isInside = [](const RgaRect& rect, const VideoFrame& frame) {
        return rect.x >= 0 && rect.y >= 0 &&
               rect.x + rect.width <= frame.width &&
               rect.y + rect.height <= frame.height;
    };
    if (!isInside(sourceRect, source) || !isInside(overlayRect, overlay) ||
        !isInside(destinationRect, destination)) {
        setError("RGA OSD 合成矩形越界");
        return false;
    }
    if (!validateWindowAlignment(source, sourceRect, "composite source rect") ||
        !validateWindowAlignment(destination, destinationRect, "composite destination rect")) {
        return false;
    }

    RgaOperation layoutOp {};
    if (!validateStrideAlignment(source, layoutOp, "composite source") ||
        !validateStrideAlignment(overlay, layoutOp, "composite overlay") ||
        !validateStrideAlignment(destination, layoutOp, "composite destination") ||
        !validateCapacity(source, "composite source") ||
        !validateCapacity(overlay, "composite overlay") ||
        !validateCapacity(destination, "composite destination")) {
        return false;
    }

    const int sourceFormat = toRgaFormat(source.format);
    const int overlayFormat = toRgaFormat(overlay.format);
    const int destinationFormat = toRgaFormat(destination.format);
    if (sourceFormat < 0 || overlayFormat < 0 || destinationFormat < 0) {
        return false;
    }

    rga_buffer_t sourceBuffer = wrapbuffer_fd_t(source.dmaFd, source.width, source.height,
                                                 effectiveStride(source), effectiveHeightStride(source),
                                                 sourceFormat);
    rga_buffer_t overlayBuffer = wrapbuffer_fd_t(overlay.dmaFd, overlay.width, overlay.height,
                                                  effectiveStride(overlay), effectiveHeightStride(overlay),
                                                  overlayFormat);
    rga_buffer_t destinationBuffer = wrapbuffer_fd_t(destination.dmaFd,
                                                      destination.width,
                                                      destination.height,
                                                      effectiveStride(destination),
                                                      effectiveHeightStride(destination),
                                                      destinationFormat);
    const im_rect sourceImRect {sourceRect.x, sourceRect.y, sourceRect.width, sourceRect.height};
    const im_rect overlayImRect {overlayRect.x, overlayRect.y, overlayRect.width, overlayRect.height};
    const im_rect destinationImRect {
        destinationRect.x,
        destinationRect.y,
        destinationRect.width,
        destinationRect.height,
    };

    const int usage = IM_SYNC | IM_ALPHA_BLEND_DST_OVER | IM_ALPHA_BLEND_PRE_MUL;
    const IM_STATUS check = imcheck_composite(sourceBuffer,
                                               destinationBuffer,
                                               overlayBuffer,
                                               sourceImRect,
                                               destinationImRect,
                                               overlayImRect,
                                               usage);
    if (check != IM_STATUS_SUCCESS && check != IM_STATUS_NOERROR) {
        setError(std::string("RGA OSD 合成 imcheck 失败: ") + imStrError(check));
        return false;
    }

    const IM_STATUS status = improcess(sourceBuffer,
                                       destinationBuffer,
                                       overlayBuffer,
                                       sourceImRect,
                                       destinationImRect,
                                       overlayImRect,
                                       -1,
                                       nullptr,
                                       nullptr,
                                       usage);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        setError(std::string("RGA OSD 合成失败: ") + imStrError(status));
        return false;
    }

    copyMeta(source, destination);
    destination.bytesUsed = requiredSize(destination);
    m_lastError.clear();
    return true;
}

bool RgaEngine::drawRectangles(VideoFrame& destination,
                               const std::vector<RgaRect>& rectangles,
                               const RgaOperation& op)
{
    if (rectangles.empty()) {
        m_lastError.clear();
        return true;
    }
    if (!validateFrame(destination, "rectangle destination") ||
        !validateStrideAlignment(destination, op, "rectangle destination") ||
        !validateCapacity(destination, "rectangle destination")) {
        return false;
    }
    if (op.rectangleLineWidth <= 0) {
        setError("RGA 矩形框线宽必须大于 0");
        return false;
    }

    std::vector<im_rect> imRects;
    imRects.reserve(rectangles.size());
    for (const RgaRect& rectangle : rectangles) {
        if (rectangle.width <= 0 || rectangle.height <= 0 ||
            rectangle.x < 0 || rectangle.y < 0 ||
            rectangle.x + rectangle.width > destination.width ||
            rectangle.y + rectangle.height > destination.height ||
            !validateWindowAlignment(destination, rectangle, "rectangle")) {
            if (m_lastError.empty()) {
                setError("RGA 矩形框越界或尺寸无效");
            }
            return false;
        }
        imRects.push_back({rectangle.x, rectangle.y, rectangle.width, rectangle.height});
    }

    const int destinationFormat = toRgaFormat(destination.format);
    if (destinationFormat < 0) {
        return false;
    }
    rga_buffer_t destinationBuffer = wrapbuffer_fd_t(destination.dmaFd,
                                                      destination.width,
                                                      destination.height,
                                                      effectiveStride(destination),
                                                      effectiveHeightStride(destination),
                                                      destinationFormat);
    const IM_STATUS status = imrectangleArray(destinationBuffer,
                                               imRects.data(),
                                               static_cast<int>(imRects.size()),
                                               op.rectangleColor,
                                               op.rectangleLineWidth,
                                               1);
    if (status != IM_STATUS_SUCCESS && status != IM_STATUS_NOERROR) {
        setError(std::string("RGA 画矩形框失败: ") + imStrError(status));
        return false;
    }

    m_lastError.clear();
    return true;
}

const std::string& RgaEngine::lastError() const
{
    return m_lastError;
}

size_t RgaEngine::bufferSizeFor(PixelFormat format,
                                int width,
                                int height,
                                int strideByteAlignment)
{
    return videoFrameBufferSizeFor(format,
                                   width,
                                   height,
                                   strideByteAlignment,
                                   VideoBufferSizeMode::Payload);
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
    case PixelFormat::RGBA8888:
        return RK_FORMAT_RGBA_8888;
    case PixelFormat::MJPEG:
        setError("RGA 不支持 MJPEG 压缩格式，需要先解码成裸帧");
        return -1;
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
    if (!validateDmaFrame(frame, name))
        return false;

    if (frame.width <= 0 || frame.height <= 0) {
        setError(std::string(name) + " 宽高无效");
        return false;
    }

    return true;
}

bool RgaEngine::validateDmaFrame(const VideoFrame& frame, const char* name)
{
    if (frame.dmaFd < 0) {
        setError(std::string(name) + " dmaFd 无效");
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

bool RgaEngine::validateWindowAlignment(const VideoFrame& frame,
                                        const RgaRect& rect,
                                        const char* name)
{
    const int alignment = minDimensionAlignment(frame.format);
    if (alignment <= 1)
        return true;

    if (rect.x % alignment != 0 || rect.y % alignment != 0 ||
        rect.width % alignment != 0 || rect.height % alignment != 0) {
        std::ostringstream oss;
        oss << name << " 不满足格式对齐: rect=["
            << rect.x << "," << rect.y << ","
            << rect.width << "," << rect.height
            << "] alignment=" << alignment;
        setError(oss.str());
        return false;
    }

    return true;
}

bool RgaEngine::computeOutputGeometry(const VideoFrame& src,
                                      const VideoFrame& dst,
                                      const RgaOperation& op,
                                      RgaGeometry& geometry)
{
    // 推导 visible 输出尺寸。这里故意不处理 stride，stride 是底层布局，
    // 会在 applyOutputGeometry()/normalizeDstLayout() 里按格式和对齐单独处理。
    const bool hasCrop = op.crop.width > 0 && op.crop.height > 0;
    if (hasCrop &&
        (op.crop.x < 0 || op.crop.y < 0 ||
         op.crop.x + op.crop.width > src.width ||
         op.crop.y + op.crop.height > src.height)) {
        std::ostringstream oss;
        oss << "RGA crop 越界: crop=[" << op.crop.x << "," << op.crop.y
            << "," << op.crop.width << "," << op.crop.height << "] src="
            << src.width << "x" << src.height;
        setError(oss.str());
        return false;
    }

    int width = hasCrop ? op.crop.width : src.width;
    int height = hasCrop ? op.crop.height : src.height;

    const bool hasRotation = op.rotation != RgaRotation::Rotate0;
    const bool autoUsesDstSize =
        op.op == RgaOp::Auto && !hasCrop && !hasRotation &&
        dst.width > 0 && dst.height > 0 &&
        (dst.width != width || dst.height != height);

    if (op.op == RgaOp::Resize || autoUsesDstSize) {
        if (dst.width > 0)
            width = dst.width;
        if (dst.height > 0)
            height = dst.height;
    }

    if (op.rotation == RgaRotation::Rotate90 || op.rotation == RgaRotation::Rotate270) {
        const int tmp = width;
        width = height;
        height = tmp;
    }

    if (width <= 0 || height <= 0) {
        setError("RGA 输出宽高无效");
        return false;
    }

    geometry.width = width;
    geometry.height = height;
    return true;
}

void RgaEngine::applyOutputGeometry(VideoFrame& dst, const RgaGeometry& geometry)
{
    // 如果调用方传入的 stride 跟旧 width 一样，认为这是“隐式紧密布局”，
    // 后续可以安全地自动对齐；如果 stride 明显不是 width，认为调用方显式指定了布局。
    const bool strideFollowsOldWidth = dst.stride <= 0 || dst.stride == dst.width;
    const bool heightStrideFollowsOldHeight =
        dst.heightStride <= 0 || dst.heightStride == dst.height;

    dst.width = geometry.width;
    dst.height = geometry.height;

    if (strideFollowsOldWidth)
        dst.stride = 0;
    if (heightStrideFollowsOldHeight)
        dst.heightStride = 0;
}

bool RgaEngine::normalizeDstLayout(VideoFrame& dst, const RgaOperation& op)
{
    // 只自动修正 dst。src 的 stride 必须来自 V4L2/MPP 等生产者，不能在这里脑补。
    if (dst.stride <= 0 || dst.stride < dst.width) {
        dst.stride = alignedStridePixels(dst.format, dst.width, op.dstStrideByteAlignment);
    }

    if (dst.heightStride <= 0 || dst.heightStride < dst.height) {
        dst.heightStride = videoFrameAlignedHeightStride(dst.format, dst.height);
    }

    return true;
}

bool RgaEngine::validateStrideAlignment(const VideoFrame& frame,
                                        const RgaOperation& op,
                                        const char* name)
{
    const int bpp = bytesPerPixel(frame.format);
    if (bpp <= 0) {
        setError(std::string(name) + " format 不支持计算 stride 对齐");
        return false;
    }

    const int dimensionAlignment = minDimensionAlignment(frame.format);
    if (frame.width % dimensionAlignment != 0 || frame.height % dimensionAlignment != 0) {
        std::ostringstream oss;
        oss << name << " visible 尺寸不满足格式对齐: visible="
            << frame.width << "x" << frame.height
            << " alignment=" << dimensionAlignment;
        setError(oss.str());
        return false;
    }

    if (effectiveStride(frame) < frame.width ||
        effectiveHeightStride(frame) < frame.height) {
        std::ostringstream oss;
        oss << name << " stride 小于 visible 尺寸: visible="
            << frame.width << "x" << frame.height
            << " stride=" << effectiveStride(frame) << "x" << effectiveHeightStride(frame);
        setError(oss.str());
        return false;
    }

    if (effectiveStride(frame) % dimensionAlignment != 0 ||
        effectiveHeightStride(frame) % dimensionAlignment != 0) {
        std::ostringstream oss;
        oss << name << " stride 不满足格式对齐: stride="
            << effectiveStride(frame) << "x" << effectiveHeightStride(frame)
            << " alignment=" << dimensionAlignment;
        setError(oss.str());
        return false;
    }

    if (op.dstStrideByteAlignment > 0 &&
        (effectiveStride(frame) * bpp) % op.dstStrideByteAlignment != 0) {
        std::ostringstream oss;
        oss << name << " 行 pitch 未按 " << op.dstStrideByteAlignment
            << " 字节对齐: stridePixels=" << effectiveStride(frame)
            << " bpp=" << bpp
            << " pitchBytes=" << effectiveStride(frame) * bpp;
        setError(oss.str());
        return false;
    }

    return true;
}

int RgaEngine::bytesPerPixel(PixelFormat format) const
{
    return videoFrameBytesPerPixelForStride(format);
}

int RgaEngine::minDimensionAlignment(PixelFormat format) const
{
    return videoFrameMinDimensionAlignment(format);
}

int RgaEngine::alignedStridePixels(PixelFormat format,
                                   int visibleWidth,
                                   int byteAlignment) const
{
    return videoFrameAlignedStride(format, visibleWidth, byteAlignment);
}

bool RgaEngine::validateSameFormat(const VideoFrame& src, const VideoFrame& dst)
{
    if (src.format != dst.format) {
        setError("当前 RgaEngine 只支持相同 PixelFormat 的 copy/resize");
        return false;
    }

    return true;
}

int RgaEngine::transformUsage(const RgaOperation& op) const
{
    int usage = 0;

    switch (op.rotation) {
    case RgaRotation::Rotate0:
        break;
    case RgaRotation::Rotate90:
        usage |= IM_HAL_TRANSFORM_ROT_90;
        break;
    case RgaRotation::Rotate180:
        usage |= IM_HAL_TRANSFORM_ROT_180;
        break;
    case RgaRotation::Rotate270:
        usage |= IM_HAL_TRANSFORM_ROT_270;
        break;
    }

    switch (op.mirror) {
    case RgaMirror::None:
        break;
    case RgaMirror::Horizontal:
        usage |= IM_HAL_TRANSFORM_FLIP_H;
        break;
    case RgaMirror::Vertical:
        usage |= IM_HAL_TRANSFORM_FLIP_V;
        break;
    case RgaMirror::Both:
        usage |= IM_HAL_TRANSFORM_FLIP_H_V;
        break;
    }

    return usage;
}

int RgaEngine::effectiveStride(const VideoFrame& frame) const
{
    // 上层没有显式指定 stride 时，按紧密排布处理。
    // 如果要 64 字节等硬件对齐，调用 RGA 前手动填 frame.stride。
    return videoFrameEffectiveStride(frame);
}

int RgaEngine::effectiveHeightStride(const VideoFrame& frame) const
{
    // V4L2 当前没有可靠的纵向 stride 就保持 0。
    // RGA 包装时把 0 当作 visible height；MPP 解码输出有 ver_stride 时再填真实值。
    return videoFrameEffectiveHeightStride(frame);
}

size_t RgaEngine::requiredSize(const VideoFrame& frame)
{
    const int widthStride = videoFrameEffectiveStride(frame);
    const int heightStride = videoFrameEffectiveHeightStride(frame);
    if (widthStride <= 0 || heightStride <= 0) {
        setError("RGA stride 无效");
        return 0;
    }

    switch (frame.format) {
    case PixelFormat::NV12:
    case PixelFormat::YUV420P:
    case PixelFormat::YUYV:
    case PixelFormat::RGBA8888:
        return videoFrameBufferSizeFor(frame.format, widthStride, heightStride);
    case PixelFormat::MJPEG:
        setError("RGA 不能计算 MJPEG 压缩格式的裸帧 buffer size，需要先解码");
        return 0;
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
    fillMeta(src, dst);
}

void RgaEngine::fillMeta(const VideoFrame& src, VideoFrame& dst)
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
