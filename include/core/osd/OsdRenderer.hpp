#pragma once

#include "DmaAllocator.hpp"
#include "FreeTypeTextRenderer.hpp"
#include "RgaEngine.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// 外部颜色统一使用直观的 RGBA，不暴露 RGA 内部使用的整数颜色布局。
struct RgbaColor {
    uint8_t red = 255;
    uint8_t green = 255;
    uint8_t blue = 255;
    uint8_t alpha = 255;
};

// Absolute 使用 OsdTextObject::left/top；其余值使用 edgeOffsetX/edgeOffsetY，
// 由 OsdRenderer 在拿到视频尺寸和文字 bitmap 尺寸后计算最终坐标。
enum class OsdAnchor {
    Absolute,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

// 一条可独立更新、独立定位的文字 OSD。
struct OsdTextObject {
    // 全局唯一 id；add/update 均通过它识别对象。
    std::string id;
    std::string text;

    // Absolute 模式：文字背景框相对视频可见区域左上角的绝对坐标，单位像素。
    // left/top 不是 stride 坐标；它们始终以 VideoFrame::width/height 为基准。
    int left = 24;
    int top = 24;

    // 边缘锚点模式：例如 TopRight + {24, 24} 表示距右边、上边各 24 像素。
    // 上层不需要知道视频宽高；最终 left/top 由 OsdRenderer 在 composite() 中计算。
    OsdAnchor anchor = OsdAnchor::Absolute;
    int edgeOffsetX = 24;
    int edgeOffsetY = 24;

    unsigned textPixelHeight = 48;
    int paddingX = 16;
    int paddingY = 12;
    RgbaColor textColor {};
    RgbaColor backgroundColor {0, 0, 0, 160};
};

// OSD 中的一组矩形框。第一版同一组框使用同一种颜色和线宽，足够表达一帧 AI
// 检测结果；不同颜色的类别以后可拆成多次 setRectangles()/drawRectangles()。
struct OsdRectangles {
    std::vector<RgaRect> rectangles;
    RgbaColor color {0, 255, 0, 255}; // 默认不透明绿色。
    int lineWidth = 4;
};

struct OsdRendererConfig {
    std::string fontPath;
};

// OSD 的职责是“管理文字对象的小 RGBA 图层并调度 RGA”，不是拥有视频输出池：
//
//   输入 NV12 source --copy--> 调用方提供的 NV12 destination
//       + 多个文字对象的小 RGBA 图层 --composite--> destination
//       + AI 框列表 --drawRectangles--> destination
//
// 文字对象在 CPU 侧各自缓存 TextBitmap；仅当文字/样式变化时才重新栅格化。首次排版时按
// 纵向范围自动分成若干条带：相近/重叠的文字共享一张“全视频宽、最小所需高度”的 RGBA
// DMA-BUF，远离的文字各自使用一条。这样既避开 RV1126B RGA 的非零横向 crop 问题，也不会
// 为顶部和底部文字申请一张接近全屏的 RGBA 画布。对象添加顺序就是同一条带内的图层顺序：
// 先添加的在底层，后添加的在上层。
class OsdRenderer {
public:
    OsdRenderer();
    ~OsdRenderer();

    OsdRenderer(const OsdRenderer&) = delete;
    OsdRenderer& operator=(const OsdRenderer&) = delete;

    // 只保存全局字体资源；文字对象及其 CPU bitmap 由 addTextObject() 单独创建。
    bool initialize(const OsdRendererConfig& config);

    // 新增一个文字对象。id 重复会失败，避免无意改变已有对象的图层顺序。
    bool addTextObject(OsdTextObject object);

    // 更新已有对象的全部配置。只改文字/颜色时仅重绘该对象所属条带；改变 top、纵向 padding
    // 或字号后，下一次 composite() 会重新计算条带布局。
    bool updateTextObject(OsdTextObject object);

    // AI 每帧更新框列表即可；这里不申请 DMA-BUF，也不做绘制。
    void setRectangles(OsdRectangles rectangles);

    // source/destination 必须是同尺寸 NV12 DMA-BUF，destination 由调用方/输出池拥有。
    // OsdRenderer 内部持有 RgaEngine，同步完成 copy + 多文字层合成 + 画框；返回时
    // destination 可直接送 MPP。
    bool composite(const VideoFrame& source, VideoFrame& destination);

    bool initialized() const;
    const std::string& lastError() const;

private:
    struct TextObjectRuntime;
    struct OverlayBand {
        DmaMemory memory;
        VideoFrame frame;
        int top = 0;
        int height = 0;
        std::vector<std::string> objectIds;
        bool dirty = true;
    };

    static bool syncForCpu(int dmaFd, bool begin, bool write, std::string& error);
    static void putRgba(uint8_t* pixels,
                        int imageWidth,
                        int x,
                        int y,
                        uint8_t red,
                        uint8_t green,
                        uint8_t blue,
                        uint8_t alpha);
    static int alignUp(int value, int alignment);
    static int alignDown(int value, int alignment);

    bool validateTextObject(const OsdTextObject& object);
    bool openTextRendererLocked(TextObjectRuntime& runtime);
    bool renderTextLocked(TextObjectRuntime& runtime);
    bool calculatePlacementLocked(TextObjectRuntime& runtime,
                                  const VideoFrame& source,
                                  RgaRect& placement);
    bool rebuildBandLayoutLocked(const VideoFrame& source);
    bool ensureBandCapacityLocked(OverlayBand& band, int requiredWidth, int requiredHeight);
    bool renderBandLocked(OverlayBand& band, const VideoFrame& source);
    void markObjectBandDirtyLocked(TextObjectRuntime& runtime);
    void setError(const std::string& message);

private:
    mutable std::mutex m_mutex;
    OsdRendererConfig m_config;
    DmaAllocator m_dmaAllocator;
    RgaEngine m_rga;
    std::unordered_map<std::string, std::unique_ptr<TextObjectRuntime>> m_textObjects;
    std::vector<std::string> m_textObjectOrder;
    std::vector<OverlayBand> m_overlayBands;
    int m_bandVideoWidth = 0;
    int m_bandVideoHeight = 0;
    bool m_bandLayoutDirty = true;
    bool m_initialized = false;
    OsdRectangles m_rectangles;
    std::string m_lastError;
};
