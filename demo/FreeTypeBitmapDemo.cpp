// OSD 封装验证 demo。
//
// 用法：
//   freetype_bitmap_demo <font.ttf> [input.png]
//
// 这个 demo 不依赖摄像头，专门验证未来 IPC 推流链的真实数据路径：
//
//   PNG RGBA -> RGA NV12 -> OsdRenderer::composite()
//   -> RGA NV12 -> RGBA -> osd_time.png
//
// OsdRenderer 内部会：
//   1. FreeType 把文字写入按纵向范围分组的“小 RGBA DMA-BUF 条带”；
//   2. RGA copy 原始 NV12 到独立输出 NV12；
//   3. RGA 将小 RGBA 文字层贴到输出；
//   4. RGA 在输出 NV12 上直接画一只模拟检测框。

#include "DmaAllocator.hpp"
#include "OsdRenderer.hpp"
#include "RgaEngine.hpp"

#include <png.h>

#include <linux/dma-buf.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr unsigned kTextPixelHeight = 48;
constexpr char kOutputFileName[] = "osd_time.png";

struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

bool readPngRgba(const std::string& path, RgbaImage& output, std::string& error)
{
    output = {};
    png_image image {};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.c_str())) {
        error = "读取 PNG 文件头失败: " + path + " (" + image.message + ")";
        return false;
    }

    image.format = PNG_FORMAT_RGBA;
    output.pixels.resize(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, output.pixels.data(), 0, nullptr)) {
        error = "解码 PNG 为 RGBA 失败: " + path + " (" + image.message + ")";
        png_image_free(&image);
        output = {};
        return false;
    }

    output.width = static_cast<int>(image.width);
    output.height = static_cast<int>(image.height);
    png_image_free(&image);
    if (output.width <= 0 || output.height <= 0 || output.pixels.empty()) {
        error = "PNG 宽高或像素数据无效";
        output = {};
        return false;
    }
    return true;
}

bool writePngRgba(const std::string& path,
                  const uint8_t* pixels,
                  int width,
                  int height,
                  std::string& error)
{
    if (pixels == nullptr || width <= 0 || height <= 0) {
        error = "输出 PNG 的 RGBA 数据无效";
        return false;
    }

    png_image image {};
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(width);
    image.height = static_cast<png_uint_32>(height);
    image.format = PNG_FORMAT_RGBA;
    if (!png_image_write_to_file(&image, path.c_str(), 0, pixels, 0, nullptr)) {
        error = "写入 PNG 失败: " + path + " (" + image.message + ")";
        return false;
    }
    return true;
}

bool syncDmaForCpu(int dmaFd, bool begin, bool write, std::string& error)
{
    dma_buf_sync sync {};
    sync.flags = (begin ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) |
                 (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ);
    if (ioctl(dmaFd, DMA_BUF_IOCTL_SYNC, &sync) == 0) {
        return true;
    }

    error = std::string("DMA_BUF_IOCTL_SYNC ") + (begin ? "START" : "END") +
            (write ? " WRITE" : " READ") + " 失败: " + std::strerror(errno);
    return false;
}

std::string formatCurrentLocalTime()
{
    const std::time_t now = std::time(nullptr);
    std::tm localTime {};
    localtime_r(&now, &localTime);

    char text[32] {};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &localTime);
    return std::string(text);
}

VideoFrame makeFrame(const DmaMemory& memory,
                     PixelFormat format,
                     int width,
                     int height)
{
    VideoFrame frame;
    frame.dmaFd = memory.fd();
    frame.va = memory.va();
    frame.capacity = memory.size();
    frame.width = width;
    frame.height = height;
    frame.stride = width;
    frame.heightStride = height;
    frame.format = format;
    frame.bytesUsed = RgaEngine::bufferSizeFor(format, width, height, 16);
    return frame;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2 || argc > 3) {
        std::cerr << "用法: " << argv[0] << " <font.ttf> [input.png]\n"
                  << "input.png 缺省时使用当前目录的 1.png；输出固定为当前目录的 "
                  << kOutputFileName << '\n';
        return 2;
    }

    const std::string fontPath = argv[1];
    const std::string inputPath = argc == 3 ? argv[2] : "1.png";
    const std::string timeText = formatCurrentLocalTime();

    RgbaImage sourceImage;
    std::string error;
    if (!readPngRgba(inputPath, sourceImage, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    // NV12 的可见宽高需偶数；RGA 这条路径的横向 stride 按 16 对齐。
    const int rgaWidth = videoFrameAlignUp(sourceImage.width, 16);
    const int rgaHeight = videoFrameAlignUp(sourceImage.height, 2);
    std::cout << "输入图片: " << inputPath << " " << sourceImage.width << 'x'
              << sourceImage.height << " RGBA，NV12 测试布局=" << rgaWidth << 'x'
              << rgaHeight << '\n';

    const size_t rgbaBytes = RgaEngine::bufferSizeFor(PixelFormat::RGBA8888,
                                                       rgaWidth,
                                                       rgaHeight,
                                                       16);
    const size_t nv12Bytes = RgaEngine::bufferSizeFor(PixelFormat::NV12,
                                                       rgaWidth,
                                                       rgaHeight,
                                                       16);
    DmaAllocator allocator;
    DmaMemory sourceRgbaDma;
    DmaMemory sourceNv12Dma;
    DmaMemory outputNv12Dma;
    DmaMemory outputRgbaDma;
    if (!allocator.allocate(rgbaBytes, sourceRgbaDma) ||
        !allocator.allocate(nv12Bytes, sourceNv12Dma) ||
        !allocator.allocate(nv12Bytes, outputNv12Dma) ||
        !allocator.allocate(rgbaBytes, outputRgbaDma)) {
        std::cerr << "申请 OSD DMA-BUF 失败: " << allocator.lastError() << '\n';
        return 1;
    }

    // CPU 写入 PNG 后显式结束 WRITE 同步；随后 RGA 才读取这块 DMA-BUF。
    if (!syncDmaForCpu(sourceRgbaDma.fd(), true, true, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    auto* sourceRgba = static_cast<uint8_t*>(sourceRgbaDma.va());
    for (int y = 0; y < rgaHeight; ++y) {
        const int sourceY = std::min(y, sourceImage.height - 1);
        uint8_t* targetRow = sourceRgba + static_cast<size_t>(y) * rgaWidth * 4;
        const uint8_t* sourceRow = sourceImage.pixels.data() +
                                   static_cast<size_t>(sourceY) * sourceImage.width * 4;
        std::memcpy(targetRow, sourceRow, static_cast<size_t>(sourceImage.width) * 4);
        for (int x = sourceImage.width; x < rgaWidth; ++x) {
            std::memcpy(targetRow + static_cast<size_t>(x) * 4,
                        targetRow + static_cast<size_t>(sourceImage.width - 1) * 4,
                        4);
        }
    }
    if (!syncDmaForCpu(sourceRgbaDma.fd(), false, true, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    VideoFrame sourceRgbaFrame = makeFrame(sourceRgbaDma, PixelFormat::RGBA8888, rgaWidth, rgaHeight);
    VideoFrame sourceNv12Frame = makeFrame(sourceNv12Dma, PixelFormat::NV12, rgaWidth, rgaHeight);
    VideoFrame outputNv12Frame = makeFrame(outputNv12Dma, PixelFormat::NV12, rgaWidth, rgaHeight);
    VideoFrame outputRgbaFrame = makeFrame(outputRgbaDma, PixelFormat::RGBA8888, rgaWidth, rgaHeight);

    RgaEngine rga;
    if (!rga.convertColor(sourceRgbaFrame, sourceNv12Frame)) {
        std::cerr << rga.lastError() << '\n';
        return 1;
    }

    OsdRenderer osd;
    OsdRendererConfig config;
    config.fontPath = fontPath;
    OsdTextObject cameraNameObject;
    cameraNameObject.id = "cameraName";
    cameraNameObject.text = "CAM-01";
    cameraNameObject.textColor = {255, 220, 64, 255};
    cameraNameObject.left = 24;
    cameraNameObject.top = 24;
    cameraNameObject.textPixelHeight = kTextPixelHeight;

    OsdTextObject timeObject;
    timeObject.id = "time";
    timeObject.text = timeText;
    // 验证上层不需要知道图片宽度：OsdRenderer 用实际 VideoFrame/bitmap 尺寸计算右上位置。
    timeObject.anchor = OsdAnchor::TopRight;
    timeObject.edgeOffsetX = 24;
    timeObject.edgeOffsetY = 24;
    timeObject.textPixelHeight = kTextPixelHeight;
    timeObject.textColor = {255, 255, 255, 255};

    OsdTextObject centerObject;
    centerObject.id = "centerStatus";
    centerObject.text = "DETECTION";
    centerObject.left = std::max(24, rgaWidth / 2 - 130);
    centerObject.top = std::max(160, rgaHeight / 2 - 40);
    centerObject.textPixelHeight = kTextPixelHeight;
    centerObject.textColor = {255, 96, 96, 255};
    if (!osd.initialize(config) ||
        !osd.addTextObject(std::move(cameraNameObject)) ||
        !osd.addTextObject(timeObject) ||
        !osd.addTextObject(std::move(centerObject))) {
        std::cerr << osd.lastError() << '\n';
        return 1;
    }

    // 模拟实际 IPC 每秒更新时钟：文字宽度变化，但对象仍处在原顶部条带内。
    // 这条更新会重新栅格化字形、只重绘 time 所属条带，不会重开字体或重新分组。
    timeObject.text += " UTC";
    if (!osd.updateTextObject(timeObject)) {
        std::cerr << osd.lastError() << '\n';
        return 1;
    }

    // 模拟一条 AI 检测结果。它只是坐标列表，OsdRenderer 在最终 NV12 输出上直接画。
    OsdRectangles simulatedDetection;
    // 故意压住中间文字：框最后画，因此最终 PNG 中绿色框线会覆盖文字/半透明背景。
    simulatedDetection.rectangles.push_back({rgaWidth / 2 - 180,
                                              std::max(140, rgaHeight / 2),
                                              360,
                                              120});
    simulatedDetection.color = {0, 255, 0, 255};
    simulatedDetection.lineWidth = 4;
    osd.setRectangles(std::move(simulatedDetection));

    if (!osd.composite(sourceNv12Frame, outputNv12Frame)) {
        std::cerr << osd.lastError() << '\n';
        return 1;
    }
    if (!rga.convertColor(outputNv12Frame, outputRgbaFrame)) {
        std::cerr << rga.lastError() << '\n';
        return 1;
    }

    // RGA 已写 outputRgbaDma；CPU 写 PNG 前需要围住 START|READ / END|READ。
    if (!syncDmaForCpu(outputRgbaDma.fd(), true, false, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    const bool written = writePngRgba(kOutputFileName,
                                      static_cast<const uint8_t*>(outputRgbaDma.va()),
                                      rgaWidth,
                                      rgaHeight,
                                      error);
    const bool endReadOk = syncDmaForCpu(outputRgbaDma.fd(), false, false, error);
    if (!written || !endReadOk) {
        std::cerr << error << '\n';
        return 1;
    }

    std::cout << "OSD 已完成：多条 RGBA 文字条带 + 与文字重叠的 NV12 检测框，输出: "
              << kOutputFileName << '\n';
    return 0;
}
