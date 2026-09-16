#include "OsdRenderer.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <utility>

#include <linux/dma-buf.h>
#include <sys/ioctl.h>

namespace {

constexpr int kBandJoinGapPixels = 16;

uint32_t toRgaColor(const RgbaColor& color)
{
    return (static_cast<uint32_t>(color.alpha) << 24U) |
           (static_cast<uint32_t>(color.red) << 16U) |
           (static_cast<uint32_t>(color.green) << 8U) |
           static_cast<uint32_t>(color.blue);
}

} // namespace

struct OsdRenderer::TextObjectRuntime {
    OsdTextObject object;
    FreeTypeTextRenderer textRenderer;
    TextBitmap bitmap;
    RgaRect placement;
    int pixelPaddingX = 0;
    int pixelPaddingY = 0;
    unsigned renderedTextPixelHeight = 0;
    size_t bandIndex = 0;
    bool hasBand = false;
    bool placementDirty = true;
};

OsdRenderer::OsdRenderer() = default;
OsdRenderer::~OsdRenderer() = default;

bool OsdRenderer::initialize(const OsdRendererConfig& config)
{
    if (config.fontPath.empty() || config.designWidth <= 0 || config.designHeight <= 0) {
        setError("OSD 配置无效：字体路径或设计分辨率不合法");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    m_textObjects.clear();
    m_textObjectOrder.clear();
    m_overlayBands.clear();
    m_bandVideoWidth = 0;
    m_bandVideoHeight = 0;
    m_bandLayoutDirty = true;
    m_rectangles = {};
    m_initialized = true;
    m_lastError.clear();
    return true;
}

bool OsdRenderer::addTextObject(OsdTextObject object)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        setError("OSD 尚未 initialize");
        return false;
    }
    if (!validateTextObject(object)) {
        return false;
    }
    if (m_textObjects.find(object.id) != m_textObjects.end()) {
        setError("OSD 文字对象 id 已存在: " + object.id);
        return false;
    }

    auto runtime = std::make_unique<TextObjectRuntime>();
    runtime->object = std::move(object);
    // addTextObject() 时还没有 VideoFrame，先按设计字号渲染；首次 composite() 会根据
    // 实际视频尺寸切到对应像素字号。1080p 默认正好无需再次打开字体。
    if (!openTextRendererLocked(*runtime, runtime->object.textPixelHeight) ||
        !renderTextLocked(*runtime)) {
        return false;
    }

    const std::string id = runtime->object.id;
    m_textObjects.emplace(id, std::move(runtime));
    m_textObjectOrder.push_back(id);
    // 新对象会影响条带归属；下一张视频帧到来时再按实际视频尺寸布局。
    m_bandLayoutDirty = true;
    m_lastError.clear();
    return true;
}

bool OsdRenderer::updateTextObject(OsdTextObject object)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        setError("OSD 尚未 initialize");
        return false;
    }
    if (!validateTextObject(object)) {
        return false;
    }

    const auto it = m_textObjects.find(object.id);
    if (it == m_textObjects.end()) {
        setError("OSD 文字对象不存在: " + object.id);
        return false;
    }

    TextObjectRuntime& runtime = *it->second;
    const OsdTextObject previous = runtime.object;
    const int oldBitmapHeight = runtime.bitmap.height;
    const bool textChanged = previous.text != object.text;
    const bool fontChanged = previous.textPixelHeight != object.textPixelHeight;
    runtime.object = std::move(object);
    if (fontChanged && !openTextRendererLocked(runtime, runtime.object.textPixelHeight)) {
        return false;
    }
    if ((textChanged || fontChanged) && !renderTextLocked(runtime)) {
        return false;
    }

    // 每条带始终覆盖视频全宽。只有纵向范围变化才需要重新分组；横向位置、文字宽度、
    // 颜色和背景只会让当前条带重绘。
    const bool layoutChanged = previous.anchor != runtime.object.anchor ||
                               previous.top != runtime.object.top ||
                               previous.edgeOffsetY != runtime.object.edgeOffsetY ||
                               previous.paddingY != runtime.object.paddingY ||
                               previous.textPixelHeight != runtime.object.textPixelHeight ||
                               oldBitmapHeight != runtime.bitmap.height;
    if (layoutChanged) {
        runtime.placementDirty = true;
        m_bandLayoutDirty = true;
    } else {
        // 文字宽度、横向位置或 paddingX 虽然不影响条带的纵向分组，但会改变它在所属
        // 条带里的实际矩形；下一次重绘该条带前重新计算一次即可。
        runtime.placementDirty = textChanged || previous.left != runtime.object.left ||
                                 previous.edgeOffsetX != runtime.object.edgeOffsetX ||
                                 previous.paddingX != runtime.object.paddingX;
        markObjectBandDirtyLocked(runtime);
    }
    m_lastError.clear();
    return true;
}

void OsdRenderer::setRectangles(OsdRectangles rectangles)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rectangles = std::move(rectangles);
}

bool OsdRenderer::composite(const VideoFrame& source, VideoFrame& destination)
{
    // RGA 是同步调用。持锁直到当前硬件操作结束，避免另一个线程改动 RGBA DMA-BUF，
    // 而 RGA 仍在读取它。
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        setError("OSD 尚未 initialize");
        return false;
    }
    if (source.format != PixelFormat::NV12 || destination.format != PixelFormat::NV12) {
        setError("OSD 当前仅支持 NV12 source/destination");
        return false;
    }

    if (!m_textObjectOrder.empty() &&
        (m_bandLayoutDirty || m_bandVideoWidth != source.width ||
         m_bandVideoHeight != source.height)) {
        if (!rebuildBandLayoutLocked(source)) {
            return false;
        }
    }

    // composite() 只写它的目标矩形，所以无论有多少条带，都必须先完整复制视频底图。
    if (!m_rga.copy(source, destination)) {
        setError("OSD 复制视频底图失败: " + m_rga.lastError());
        return false;
    }

    // 一条带就是一次局部 RGA alpha 合成。上/中/下文字相距很远时各用小条带，
    // 不会为了它们申请全屏 RGBA 画布。
    for (OverlayBand& band : m_overlayBands) {
        if (band.dirty && !renderBandLocked(band, source)) {
            return false;
        }

        RgaOperation operation;
        operation.crop = {0, band.top, source.width, band.height};
        operation.overlayCrop = {0, 0, source.width, band.height};
        operation.compositeDestination = operation.crop;
        if (!m_rga.composite(source, band.frame, destination, operation)) {
            setError("OSD 合成文字条带失败: " + m_rga.lastError());
            return false;
        }
    }

    // 框最后画，所以检测框与文字重叠时，框线会盖在文字/文字背景上。
    if (!m_rectangles.rectangles.empty()) {
        RgaOperation operation;
        operation.rectangleColor = toRgaColor(m_rectangles.color);
        // 检测框坐标已是当前视频坐标，不能缩放；但线宽是视觉样式，按 OSD 设计比例缩放。
        operation.rectangleLineWidth = std::max(1, scaleDesignPixelsLocked(m_rectangles.lineWidth, source));
        if (!m_rga.drawRectangles(destination, m_rectangles.rectangles, operation)) {
            setError("OSD 画检测框失败: " + m_rga.lastError());
            return false;
        }
    }

    m_lastError.clear();
    return true;
}

bool OsdRenderer::initialized() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_initialized;
}

const std::string& OsdRenderer::lastError() const
{
    return m_lastError;
}

bool OsdRenderer::syncForCpu(int dmaFd, bool begin, bool write, std::string& error)
{
    dma_buf_sync sync {};
    sync.flags = (begin ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) |
                 (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ);
    if (ioctl(dmaFd, DMA_BUF_IOCTL_SYNC, &sync) == 0) {
        return true;
    }
    error = std::string("OSD DMA_BUF_IOCTL_SYNC ") + (begin ? "START" : "END") +
            (write ? " WRITE" : " READ") + " 失败: " + std::strerror(errno);
    return false;
}

void OsdRenderer::putRgba(uint8_t* pixels,
                          int imageWidth,
                          int x,
                          int y,
                          uint8_t red,
                          uint8_t green,
                          uint8_t blue,
                          uint8_t alpha)
{
    uint8_t* destination = pixels + (static_cast<size_t>(y) * imageWidth + x) * 4;
    destination[0] = red;
    destination[1] = green;
    destination[2] = blue;
    destination[3] = alpha;
}

int OsdRenderer::alignUp(int value, int alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

int OsdRenderer::alignDown(int value, int alignment)
{
    return value / alignment * alignment;
}

bool OsdRenderer::validateTextObject(const OsdTextObject& object)
{
    if (object.id.empty() || object.text.empty() || object.textPixelHeight == 0 ||
        object.left < 0 || object.top < 0 || object.edgeOffsetX < 0 || object.edgeOffsetY < 0 ||
        object.paddingX < 0 || object.paddingY < 0) {
        setError("OSD 文字对象配置无效：id/文字/字号/坐标/padding 不合法");
        return false;
    }
    return true;
}

bool OsdRenderer::openTextRendererLocked(TextObjectRuntime& runtime, unsigned pixelHeight)
{
    if (!runtime.textRenderer.open(m_config.fontPath, pixelHeight)) {
        setError("OSD 打开字体失败 id=" + runtime.object.id + ": " +
                 runtime.textRenderer.lastError());
        return false;
    }
    runtime.renderedTextPixelHeight = pixelHeight;
    return true;
}

bool OsdRenderer::renderTextLocked(TextObjectRuntime& runtime)
{
    if (!runtime.textRenderer.renderText(runtime.object.text, runtime.bitmap)) {
        setError("OSD 渲染文字失败 id=" + runtime.object.id + ": " +
                 runtime.textRenderer.lastError());
        return false;
    }
    return true;
}

double OsdRenderer::scaleForSourceLocked(const VideoFrame& source) const
{
    const double widthScale = static_cast<double>(source.width) / m_config.designWidth;
    const double heightScale = static_cast<double>(source.height) / m_config.designHeight;
    return std::min(widthScale, heightScale);
}

int OsdRenderer::scaleDesignPixelsLocked(int designPixels, const VideoFrame& source) const
{
    return static_cast<int>(std::lround(designPixels * scaleForSourceLocked(source)));
}

bool OsdRenderer::ensureTextBitmapForSourceLocked(TextObjectRuntime& runtime,
                                                   const VideoFrame& source)
{
    const int scaledHeight = scaleDesignPixelsLocked(
        static_cast<int>(runtime.object.textPixelHeight), source);
    const unsigned pixelHeight = static_cast<unsigned>(std::max(1, scaledHeight));
    if (runtime.bitmap.valid() && runtime.renderedTextPixelHeight == pixelHeight) {
        return true;
    }

    // 只有首次看到非设计分辨率、或运行中视频尺寸跨越字号边界时才重开字体；正常 30fps
    // 路径只做一次整数比较。时钟更新会复用这份 face，直接 renderText() 即可。
    if (!openTextRendererLocked(runtime, pixelHeight) || !renderTextLocked(runtime)) {
        return false;
    }
    runtime.placementDirty = true;
    return true;
}

bool OsdRenderer::calculatePlacementLocked(TextObjectRuntime& runtime,
                                           const VideoFrame& source,
                                           RgaRect& placement)
{
    if (!ensureTextBitmapForSourceLocked(runtime, source)) {
        return false;
    }

    runtime.pixelPaddingX = std::max(0, scaleDesignPixelsLocked(runtime.object.paddingX, source));
    runtime.pixelPaddingY = std::max(0, scaleDesignPixelsLocked(runtime.object.paddingY, source));

    // NV12 的合成目标必须偶数对齐。对象配置的坐标、边距和 padding 都是设计像素，
    // 先按实际视频尺寸换算，再向左/上取偶数满足 YUV420 chroma 对齐。
    placement.width = alignUp(runtime.bitmap.width + runtime.pixelPaddingX * 2, 16);
    placement.height = alignUp(runtime.bitmap.height + runtime.pixelPaddingY * 2, 2);
    int left = scaleDesignPixelsLocked(runtime.object.left, source);
    int top = scaleDesignPixelsLocked(runtime.object.top, source);
    switch (runtime.object.anchor) {
    case OsdAnchor::Absolute:
        break;
    case OsdAnchor::TopLeft:
        left = scaleDesignPixelsLocked(runtime.object.edgeOffsetX, source);
        top = scaleDesignPixelsLocked(runtime.object.edgeOffsetY, source);
        break;
    case OsdAnchor::TopRight:
        left = source.width - scaleDesignPixelsLocked(runtime.object.edgeOffsetX, source) - placement.width;
        top = scaleDesignPixelsLocked(runtime.object.edgeOffsetY, source);
        break;
    case OsdAnchor::BottomLeft:
        left = scaleDesignPixelsLocked(runtime.object.edgeOffsetX, source);
        top = source.height - scaleDesignPixelsLocked(runtime.object.edgeOffsetY, source) - placement.height;
        break;
    case OsdAnchor::BottomRight:
        left = source.width - scaleDesignPixelsLocked(runtime.object.edgeOffsetX, source) - placement.width;
        top = source.height - scaleDesignPixelsLocked(runtime.object.edgeOffsetY, source) - placement.height;
        break;
    }
    placement.x = alignDown(left, 2);
    placement.y = alignDown(top, 2);
    if (placement.width <= 0 || placement.height <= 0 || placement.x < 0 || placement.y < 0 ||
        placement.x + placement.width > source.width ||
        placement.y + placement.height > source.height) {
        setError("OSD 文字对象超出视频范围 id=" + runtime.object.id + " video=" +
                 std::to_string(source.width) + "x" + std::to_string(source.height));
        return false;
    }
    return true;
}

bool OsdRenderer::rebuildBandLayoutLocked(const VideoFrame& source)
{
    struct LayoutItem {
        TextObjectRuntime* runtime = nullptr;
        RgaRect placement;
        size_t order = 0;
    };

    std::vector<LayoutItem> items;
    items.reserve(m_textObjectOrder.size());
    for (size_t order = 0; order < m_textObjectOrder.size(); ++order) {
        const auto it = m_textObjects.find(m_textObjectOrder[order]);
        if (it == m_textObjects.end()) {
            continue;
        }
        RgaRect placement;
        if (!calculatePlacementLocked(*it->second, source, placement)) {
            return false;
        }
        items.push_back({it->second.get(), placement, order});
    }
    if (items.empty()) {
        setError("OSD 没有可合成的文字对象");
        return false;
    }

    std::sort(items.begin(), items.end(), [](const LayoutItem& left, const LayoutItem& right) {
        if (left.placement.y != right.placement.y) {
            return left.placement.y < right.placement.y;
        }
        return left.order < right.order;
    });

    // 条带合并间距也是设计像素：720p 时 16px 设计间距约为 11px，保持与 1080p
    // 一致的布局判断，而不是因为分辨率降低意外合并更多文字。
    const int bandJoinGapPixels = std::max(0, scaleDesignPixelsLocked(kBandJoinGapPixels, source));
    std::vector<OverlayBand> bands;
    for (const LayoutItem& item : items) {
        const int itemBottom = item.placement.y + item.placement.height;
        if (bands.empty() || item.placement.y > bands.back().top + bands.back().height +
                                                  bandJoinGapPixels) {
            OverlayBand band;
            band.top = item.placement.y;
            band.height = item.placement.height;
            band.objectIds.push_back(item.runtime->object.id);
            bands.push_back(std::move(band));
        } else {
            OverlayBand& band = bands.back();
            const int bandBottom = std::max(band.top + band.height, itemBottom);
            band.height = bandBottom - band.top;
            band.objectIds.push_back(item.runtime->object.id);
        }
    }

    for (size_t bandIndex = 0; bandIndex < bands.size(); ++bandIndex) {
        OverlayBand& band = bands[bandIndex];
        band.top = alignDown(band.top, 2);
        band.height = alignUp(band.height, 2);
        band.dirty = true;
        for (const std::string& id : band.objectIds) {
            TextObjectRuntime& runtime = *m_textObjects.at(id);
            if (!calculatePlacementLocked(runtime, source, runtime.placement)) {
                return false;
            }
            runtime.bandIndex = bandIndex;
            runtime.hasBand = true;
            runtime.placementDirty = false;
        }
        if (!ensureBandCapacityLocked(band, source.width, band.height)) {
            return false;
        }
    }

    m_overlayBands = std::move(bands);
    m_bandVideoWidth = source.width;
    m_bandVideoHeight = source.height;
    m_bandLayoutDirty = false;
    return true;
}

bool OsdRenderer::ensureBandCapacityLocked(OverlayBand& band,
                                           int requiredWidth,
                                           int requiredHeight)
{
    if (requiredWidth <= 0 || requiredHeight <= 0) {
        setError("OSD 文字条带尺寸无效");
        return false;
    }
    if (band.memory.valid() && band.frame.width >= requiredWidth &&
        band.frame.height >= requiredHeight) {
        band.frame.width = requiredWidth;
        band.frame.height = requiredHeight;
        band.frame.stride = requiredWidth;
        band.frame.heightStride = requiredHeight;
        band.frame.bytesUsed = static_cast<size_t>(requiredWidth) * requiredHeight * 4;
        return true;
    }

    const size_t byteSize = static_cast<size_t>(requiredWidth) * requiredHeight * 4;
    DmaMemory memory;
    if (!m_dmaAllocator.allocate(byteSize, memory)) {
        setError("OSD 申请 RGBA 文字条带 DMA-BUF 失败: " + m_dmaAllocator.lastError());
        return false;
    }

    band.memory = std::move(memory);
    band.frame = {};
    band.frame.dmaFd = band.memory.fd();
    band.frame.va = band.memory.va();
    band.frame.capacity = band.memory.size();
    band.frame.bytesUsed = byteSize;
    band.frame.width = requiredWidth;
    band.frame.height = requiredHeight;
    band.frame.stride = requiredWidth;
    band.frame.heightStride = requiredHeight;
    band.frame.format = PixelFormat::RGBA8888;
    return true;
}

bool OsdRenderer::renderBandLocked(OverlayBand& band, const VideoFrame& source)
{
    std::string syncError;
    if (!syncForCpu(band.memory.fd(), true, true, syncError)) {
        setError(syncError);
        return false;
    }

    auto* pixels = static_cast<uint8_t*>(band.memory.va());
    std::memset(pixels, 0, band.memory.size());
    for (const std::string& id : band.objectIds) {
        const auto it = m_textObjects.find(id);
        if (it == m_textObjects.end()) {
            continue;
        }
        TextObjectRuntime& runtime = *it->second;
        if (runtime.placementDirty) {
            RgaRect placement;
            if (!calculatePlacementLocked(runtime, source, placement)) {
                return false;
            }
            // 此分支只允许横向或文字宽度变化；纵向变化已经在 updateTextObject() 里
            // 触发重分组。这里再防御一次，避免错误地写出本条带。
            if (placement.y < band.top ||
                placement.y + placement.height > band.top + band.height) {
                setError("OSD 文字对象的纵向范围已变化但未重建条带 id=" + runtime.object.id);
                return false;
            }
            runtime.placement = placement;
            runtime.placementDirty = false;
        }
        const int originX = runtime.placement.x;
        const int originY = runtime.placement.y - band.top;

        for (int y = 0; y < runtime.placement.height; ++y) {
            for (int x = 0; x < runtime.placement.width; ++x) {
                putRgba(pixels,
                        band.frame.stride,
                        originX + x,
                        originY + y,
                        runtime.object.backgroundColor.red,
                        runtime.object.backgroundColor.green,
                        runtime.object.backgroundColor.blue,
                        runtime.object.backgroundColor.alpha);
            }
        }

        // FreeType 给的是 glyph coverage；保持 RGB 为外部传入的普通 RGBA，仅缩放 alpha。
        // RGA composite() 使用普通 alpha 路径，所以 CPU 不做 RGB 预乘。
        for (int y = 0; y < runtime.bitmap.height; ++y) {
            for (int x = 0; x < runtime.bitmap.width; ++x) {
                const uint8_t coverage = runtime.bitmap.alpha[
                    static_cast<size_t>(y) * runtime.bitmap.width + x];
                if (coverage == 0) {
                    continue;
                }
                const uint8_t alpha = static_cast<uint8_t>(
                    static_cast<unsigned>(coverage) * runtime.object.textColor.alpha / 255U);
                putRgba(pixels,
                        band.frame.stride,
                        originX + runtime.pixelPaddingX + x,
                        originY + runtime.pixelPaddingY + y,
                        runtime.object.textColor.red,
                        runtime.object.textColor.green,
                        runtime.object.textColor.blue,
                        alpha);
            }
        }
    }

    if (!syncForCpu(band.memory.fd(), false, true, syncError)) {
        setError(syncError);
        return false;
    }
    band.dirty = false;
    return true;
}

void OsdRenderer::markObjectBandDirtyLocked(TextObjectRuntime& runtime)
{
    if (runtime.hasBand && runtime.bandIndex < m_overlayBands.size()) {
        m_overlayBands[runtime.bandIndex].dirty = true;
    } else {
        // 第一次 composite() 前还没有视频尺寸和条带归属，留给布局阶段统一处理。
        m_bandLayoutDirty = true;
    }
}

void OsdRenderer::setError(const std::string& message)
{
    m_lastError = message;
}
