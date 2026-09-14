#pragma once

#include "DmaAllocator.hpp"
#include "FreeTypeTextRenderer.hpp"
#include "RgaEngine.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// OSD 中的一组矩形框。第一版同一组框使用同一种颜色和线宽，足够表达一帧 AI
// 检测结果；不同颜色的类别以后可拆成多次 setRectangles()/drawRectangles()。
struct OsdRectangles {
    std::vector<RgaRect> rectangles;
    uint32_t color = 0xFF00FF00; // 0xAARRGGBB，默认不透明绿色。
    int lineWidth = 4;
};

struct OsdRendererConfig {
    std::string fontPath;
    unsigned textPixelHeight = 48;

    // 这是“小 RGBA 文字层”的固定 DMA-BUF 布局，不是整张视频画面。
    // 调用方按最长可能文字配置即可；宽至少 16 对齐、高至少偶数，便于贴到 NV12。
    int overlayWidth = 768;
    int overlayHeight = 96;
    int overlayLeft = 24;
    int overlayTop = 24;
    int textPaddingX = 16;
    int textPaddingY = 12;
    uint8_t textBackgroundAlpha = 160;
};

// OSD 的职责是“准备覆盖层并调度 RGA”，不是拥有视频输出池：
//
//   输入 NV12 source --copy--> 调用方提供的 NV12 destination
//       + 小 RGBA 文字层 --composite--> destination
//       + AI 框列表 --drawRectangles--> destination
//
// 文本层使用两个小 DMA-BUF：CPU 写下一版文字时，RGA 仍可读取上一版文字。
// 目前 RtspPublishSink 每路只有一个编码 worker，updateText()/composite() 在同一
// worker 中串行调用；内部 mutex 只是保证未来控制线程更新文字时的状态快照安全。
class OsdRenderer {
public:
    OsdRenderer() = default;

    OsdRenderer(const OsdRenderer&) = delete;
    OsdRenderer& operator=(const OsdRenderer&) = delete;

    bool initialize(const OsdRendererConfig& config);

    // CPU 将 UTF-8 文本栅格化进“下一块”RGBA DMA-BUF，并在写完后完成 DMA cache sync。
    // 文本超出 config.overlayWidth/overlayHeight 时返回 false；调用方据此调整布局，
    // 而不是在热路径临时扩容/换 DMA-BUF。
    bool updateText(const std::string& utf8Text);

    // AI 每帧更新框列表即可；这里不申请 DMA-BUF，也不做绘制。
    void setRectangles(OsdRectangles rectangles);

    // source/destination 必须是同尺寸 NV12 DMA-BUF，destination 由调用方/输出池拥有。
    // 该函数同步完成 copy + 文本合成 + 画框，返回时 destination 可直接送 MPP。
    bool composite(const VideoFrame& source, VideoFrame& destination, RgaEngine& rga);

    bool initialized() const;
    const std::string& lastError() const;

private:
    struct OverlayBuffer {
        DmaMemory memory;
        VideoFrame frame;
    };

    static bool syncForCpu(int dmaFd, bool begin, bool write, std::string& error);
    static void putPremultipliedRgba(uint8_t* pixels,
                                     int imageWidth,
                                     int x,
                                     int y,
                                     uint8_t red,
                                     uint8_t green,
                                     uint8_t blue,
                                     uint8_t alpha);
    bool writeTextToOverlayLocked(const TextBitmap& text, OverlayBuffer& overlay);
    void setError(const std::string& message);

private:
    mutable std::mutex m_mutex;
    OsdRendererConfig m_config;
    FreeTypeTextRenderer m_textRenderer;
    DmaAllocator m_dmaAllocator;
    std::array<OverlayBuffer, 2> m_textOverlays;
    unsigned m_activeOverlayIndex = 0;
    bool m_hasText = false;
    bool m_initialized = false;
    OsdRectangles m_rectangles;
    std::string m_lastError;
};
