#include "OsdRenderer.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <linux/dma-buf.h>
#include <sys/ioctl.h>

bool OsdRenderer::initialize(const OsdRendererConfig& config)
{
    if (config.fontPath.empty() || config.textPixelHeight == 0 ||
        config.overlayWidth <= 0 || config.overlayHeight <= 0 ||
        config.overlayWidth % 16 != 0 || config.overlayHeight % 2 != 0 ||
        config.overlayLeft < 0 || config.overlayTop < 0 ||
        config.textPaddingX < 0 || config.textPaddingY < 0) {
        setError("OSD 配置无效：字体/字号/overlay 尺寸或位置不合法");
        return false;
    }
    if (!m_textRenderer.open(config.fontPath, config.textPixelHeight)) {
        setError("OSD 打开字体失败: " + m_textRenderer.lastError());
        return false;
    }

    const size_t bufferSize = static_cast<size_t>(config.overlayWidth) *
                              static_cast<size_t>(config.overlayHeight) * 4;
    std::array<OverlayBuffer, 2> newOverlays;
    for (OverlayBuffer& overlay : newOverlays) {
        if (!m_dmaAllocator.allocate(bufferSize, overlay.memory)) {
            setError("OSD 申请 RGBA overlay DMA-BUF 失败: " + m_dmaAllocator.lastError());
            return false;
        }
        overlay.frame.dmaFd = overlay.memory.fd();
        overlay.frame.va = overlay.memory.va();
        overlay.frame.capacity = overlay.memory.size();
        overlay.frame.bytesUsed = bufferSize;
        overlay.frame.width = config.overlayWidth;
        overlay.frame.height = config.overlayHeight;
        overlay.frame.stride = config.overlayWidth;
        overlay.frame.heightStride = config.overlayHeight;
        overlay.frame.format = PixelFormat::RGBA8888;

        std::string syncError;
        if (!syncForCpu(overlay.memory.fd(), true, true, syncError)) {
            setError(syncError);
            return false;
        }
        std::memset(overlay.memory.va(), 0, bufferSize);
        if (!syncForCpu(overlay.memory.fd(), false, true, syncError)) {
            setError(syncError);
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    m_textOverlays = std::move(newOverlays);
    m_activeOverlayIndex = 0;
    m_hasText = false;
    m_rectangles = {};
    m_initialized = true;
    m_lastError.clear();
    return true;
}

bool OsdRenderer::updateText(const std::string& utf8Text)
{
    TextBitmap text;
    if (!m_textRenderer.renderText(utf8Text, text)) {
        setError("OSD 渲染文字失败: " + m_textRenderer.lastError());
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        setError("OSD 尚未 initialize");
        return false;
    }
    const int boxWidth = text.width + m_config.textPaddingX * 2;
    const int boxHeight = text.height + m_config.textPaddingY * 2;
    if (boxWidth > m_config.overlayWidth || boxHeight > m_config.overlayHeight) {
        setError("OSD 文本超出 overlay 布局: text=" + std::to_string(text.width) + "x" +
                 std::to_string(text.height) + " overlay=" +
                 std::to_string(m_config.overlayWidth) + "x" +
                 std::to_string(m_config.overlayHeight));
        return false;
    }

    const unsigned writeIndex = m_hasText ? (m_activeOverlayIndex + 1) % m_textOverlays.size() : 0;
    if (!writeTextToOverlayLocked(text, m_textOverlays[writeIndex])) {
        return false;
    }
    m_activeOverlayIndex = writeIndex;
    m_hasText = true;
    m_lastError.clear();
    return true;
}

void OsdRenderer::setRectangles(OsdRectangles rectangles)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rectangles = std::move(rectangles);
}

bool OsdRenderer::composite(const VideoFrame& source, VideoFrame& destination, RgaEngine& rga)
{
    // RGA 是同步调用。持锁直到这次硬件操作结束，避免控制线程 updateText() 把
    // 同一个双缓冲 DMA-BUF 改写，而 RGA 尚在读取它。文字只按秒更新，这个临界区
    // 不在逐像素 CPU 热路径上，也不会阻塞采集/编码 worker 之外的工作。
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        setError("OSD 尚未 initialize");
        return false;
    }
    OverlayBuffer* activeOverlay = m_hasText ? &m_textOverlays[m_activeOverlayIndex] : nullptr;

    if (!rga.copy(source, destination)) {
        setError("OSD 复制视频底图失败: " + rga.lastError());
        return false;
    }

    if (activeOverlay != nullptr) {
        RgaOperation operation;
        operation.crop = {m_config.overlayLeft,
                          m_config.overlayTop,
                          m_config.overlayWidth,
                          m_config.overlayHeight};
        operation.overlayCrop = {0, 0, m_config.overlayWidth, m_config.overlayHeight};
        operation.compositeDestination = operation.crop;
        if (!rga.composite(source, activeOverlay->frame, destination, operation)) {
            setError("OSD 合成文字失败: " + rga.lastError());
            return false;
        }
    }

    if (!m_rectangles.rectangles.empty()) {
        RgaOperation operation;
        operation.rectangleColor = m_rectangles.color;
        operation.rectangleLineWidth = m_rectangles.lineWidth;
        if (!rga.drawRectangles(destination, m_rectangles.rectangles, operation)) {
            setError("OSD 画检测框失败: " + rga.lastError());
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

void OsdRenderer::putPremultipliedRgba(uint8_t* pixels,
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

bool OsdRenderer::writeTextToOverlayLocked(const TextBitmap& text, OverlayBuffer& overlay)
{
    std::string syncError;
    if (!syncForCpu(overlay.memory.fd(), true, true, syncError)) {
        setError(syncError);
        return false;
    }

    auto* pixels = static_cast<uint8_t*>(overlay.memory.va());
    const size_t byteSize = static_cast<size_t>(m_config.overlayWidth) * m_config.overlayHeight * 4;
    std::memset(pixels, 0, byteSize);

    const int boxWidth = text.width + m_config.textPaddingX * 2;
    const int boxHeight = text.height + m_config.textPaddingY * 2;
    for (int y = 0; y < boxHeight; ++y) {
        for (int x = 0; x < boxWidth; ++x) {
            putPremultipliedRgba(pixels,
                                 m_config.overlayWidth,
                                 x,
                                 y,
                                 0,
                                 0,
                                 0,
                                 m_config.textBackgroundAlpha);
        }
    }
    for (int y = 0; y < text.height; ++y) {
        for (int x = 0; x < text.width; ++x) {
            const uint8_t alpha = text.alpha[static_cast<size_t>(y) * text.width + x];
            if (alpha != 0) {
                putPremultipliedRgba(pixels,
                                     m_config.overlayWidth,
                                     x + m_config.textPaddingX,
                                     y + m_config.textPaddingY,
                                     alpha,
                                     alpha,
                                     alpha,
                                     alpha);
            }
        }
    }

    if (!syncForCpu(overlay.memory.fd(), false, true, syncError)) {
        setError(syncError);
        return false;
    }
    return true;
}

void OsdRenderer::setError(const std::string& message)
{
    m_lastError = message;
}
