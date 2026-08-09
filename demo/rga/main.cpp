#include "DmaAllocator.hpp"
#include "VideoFrame.hpp"
#include "hw/RgaEngine.hpp"

#include <QImage>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <linux/dma-buf.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kPoolStrideAlignment = 64;

int alignUp(int value, int alignment)
{
    if (alignment <= 1)
        return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

int bytesPerPixel(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12:
    case PixelFormat::YUV420P:
        return 1;
    case PixelFormat::YUYV:
        return 2;
    case PixelFormat::RGBA8888:
        return 4;
    case PixelFormat::Unknown:
    case PixelFormat::Auto:
        return 0;
    }
    return 0;
}

int minDimensionAlignment(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12:
    case PixelFormat::YUV420P:
    case PixelFormat::YUYV:
        return 2;
    case PixelFormat::RGBA8888:
    case PixelFormat::Unknown:
    case PixelFormat::Auto:
        return 1;
    }
    return 1;
}

int alignedStridePixels(PixelFormat format, int width, int byteAlignment)
{
    const int bpp = bytesPerPixel(format);
    if (bpp <= 0)
        return width;

    int stride = alignUp(width, minDimensionAlignment(format));
    if (byteAlignment > 0) {
        const int pitch = alignUp(stride * bpp, byteAlignment);
        stride = alignUp((pitch + bpp - 1) / bpp, minDimensionAlignment(format));
    }
    return stride;
}

void dmabufSync(int fd, unsigned long flags)
{
    if (fd < 0)
        return;

    dma_buf_sync sync {};
    sync.flags = flags;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

std::string executableDir()
{
    char path[4096] {};
    const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0)
        return ".";
    path[len] = '\0';

    std::string full(path);
    const std::string::size_type slash = full.find_last_of('/');
    if (slash == std::string::npos)
        return ".";
    return full.substr(0, slash);
}

bool fileExists(const std::string& path)
{
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string defaultInputPath(const std::string& exeDir)
{
    const std::vector<std::string> candidates {
        exeDir + "/../../img/1.png",
        exeDir + "/../img/1.png",
        exeDir + "/img/1.png",
        "/home/alientek/MultiCamRenderer/img/1.png",
    };

    for (const std::string& path : candidates) {
        if (fileExists(path))
            return path;
    }
    return candidates.front();
}

struct OwnedFrame {
    DmaMemory memory;
    VideoFrame frame;
};

bool allocateFrame(DmaAllocator& allocator,
                   PixelFormat format,
                   int width,
                   int height,
                   OwnedFrame& out,
                   int strideByteAlignment = kPoolStrideAlignment)
{
    const size_t size = RgaEngine::bufferSizeFor(format, width, height, strideByteAlignment);
    if (size == 0) {
        std::cerr << "bufferSizeFor failed: " << width << "x" << height << "\n";
        return false;
    }

    if (!allocator.allocate(size, out.memory)) {
        std::cerr << "DMA allocate failed: " << allocator.lastError() << "\n";
        return false;
    }

    out.frame.dmaFd = out.memory.fd();
    out.frame.va = out.memory.va();
    out.frame.capacity = out.memory.size();
    out.frame.width = width;
    out.frame.height = height;
    out.frame.stride = alignedStridePixels(format, width, strideByteAlignment);
    out.frame.heightStride = alignUp(height, minDimensionAlignment(format));
    out.frame.format = format;
    out.frame.bytesUsed = RgaEngine::bufferSizeFor(format, width, height, strideByteAlignment);
    return true;
}

bool uploadRgba(const QImage& image, OwnedFrame& dst)
{
    if (dst.frame.format != PixelFormat::RGBA8888)
        return false;

    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.width() != dst.frame.width || rgba.height() != dst.frame.height)
        return false;

    dmabufSync(dst.frame.dmaFd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    auto* base = static_cast<unsigned char*>(dst.frame.va);
    const int dstPitch = dst.frame.stride * 4;
    const int copyBytes = dst.frame.width * 4;
    for (int y = 0; y < dst.frame.height; ++y) {
        std::memcpy(base + y * dstPitch, rgba.constScanLine(y), static_cast<size_t>(copyBytes));
        if (dstPitch > copyBytes) {
            std::memset(base + y * dstPitch + copyBytes, 0, static_cast<size_t>(dstPitch - copyBytes));
        }
    }
    dmabufSync(dst.frame.dmaFd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    return true;
}

bool saveRgba(const VideoFrame& frame, const std::string& path)
{
    if (frame.format != PixelFormat::RGBA8888 || frame.va == nullptr) {
        std::cerr << "saveRgba requires RGBA8888 frame\n";
        return false;
    }

    dmabufSync(frame.dmaFd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    const auto* data = static_cast<const unsigned char*>(frame.va);
    QImage image(data,
                 frame.width,
                 frame.height,
                 frame.stride * 4,
                 QImage::Format_RGBA8888);
    const bool ok = image.copy().save(QString::fromStdString(path), "PNG");
    dmabufSync(frame.dmaFd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);

    if (!ok)
        std::cerr << "save failed: " << path << "\n";
    return ok;
}

bool runRgaCase(RgaEngine& rga,
                DmaAllocator& allocator,
                const VideoFrame& src,
                const RgaOperation& op,
                PixelFormat dstFormat,
                int allocWidth,
                int allocHeight,
                const std::string& outputPath)
{
    OwnedFrame dst {};
    if (!allocateFrame(allocator, dstFormat, allocWidth, allocHeight, dst))
        return false;

    if (!rga.rga(src, dst.frame, op)) {
        std::cerr << "RGA case failed: " << outputPath << "\n"
                  << "  " << rga.lastError() << "\n";
        return false;
    }

    if (dst.frame.format == PixelFormat::RGBA8888)
        return saveRgba(dst.frame, outputPath);

    return true;
}

bool runConvertRoundTrip(RgaEngine& rga,
                         DmaAllocator& allocator,
                         const VideoFrame& src,
                         const std::string& outputPath)
{
    const int evenWidth = src.width & ~1;
    const int evenHeight = src.height & ~1;
    if (evenWidth <= 0 || evenHeight <= 0)
        return false;

    RgaOperation toYuyv {};
    toYuyv.op = RgaOp::ConvertColor;
    toYuyv.crop = RgaRect {0, 0, evenWidth, evenHeight};

    OwnedFrame yuyv {};
    if (!allocateFrame(allocator, PixelFormat::YUYV, evenWidth, evenHeight, yuyv))
        return false;

    if (!rga.rga(src, yuyv.frame, toYuyv)) {
        std::cerr << "RGBA -> YUYV failed: " << rga.lastError() << "\n";
        return false;
    }

    OwnedFrame rgba {};
    if (!allocateFrame(allocator, PixelFormat::RGBA8888, evenWidth, evenHeight, rgba))
        return false;

    RgaOperation backToRgba {};
    backToRgba.op = RgaOp::ConvertColor;
    if (!rga.rga(yuyv.frame, rgba.frame, backToRgba)) {
        std::cerr << "YUYV -> RGBA failed: " << rga.lastError() << "\n";
        return false;
    }

    return saveRgba(rgba.frame, outputPath);
}

} // namespace

int main(int argc, char** argv)
{
    const std::string exeDir = executableDir();
    const std::string inputPath = argc > 1 ? argv[1] : defaultInputPath(exeDir);

    QImage input(QString::fromStdString(inputPath));
    if (input.isNull()) {
        std::cerr << "load image failed: " << inputPath << "\n";
        return 1;
    }
    input = input.convertToFormat(QImage::Format_RGBA8888);

    std::cout << "input: " << inputPath << " " << input.width() << "x" << input.height() << "\n";

    DmaAllocator allocator;
    OwnedFrame src {};
    if (!allocateFrame(allocator, PixelFormat::RGBA8888, input.width(), input.height(), src))
        return 1;
    if (!uploadRgba(input, src)) {
        std::cerr << "upload image to DMA buffer failed\n";
        return 1;
    }

    RgaEngine rga;
    bool ok = true;

    auto out = [&](const char* name) {
        return exeDir + "/" + name;
    };

    ok &= runRgaCase(rga, allocator, src.frame, RgaOperation {RgaOp::Copy},
                     PixelFormat::RGBA8888, input.width(), input.height(), out("out_01_copy.png"));

    OwnedFrame resizeDst {};
    RgaOperation resize {};
    resize.op = RgaOp::Resize;
    ok &= runRgaCase(rga, allocator, src.frame, resize,
                     PixelFormat::RGBA8888, input.width() / 2, input.height() / 2, out("out_02_resize_half.png"));

    RgaOperation crop {};
    crop.op = RgaOp::Copy;
    crop.crop = RgaRect {input.width() / 4, input.height() / 4, input.width() / 2, input.height() / 2};
    ok &= runRgaCase(rga, allocator, src.frame, crop,
                     PixelFormat::RGBA8888, crop.crop.width, crop.crop.height, out("out_03_crop_center.png"));

    RgaOperation rot90 {};
    rot90.op = RgaOp::Copy;
    rot90.rotation = RgaRotation::Rotate90;
    ok &= runRgaCase(rga, allocator, src.frame, rot90,
                     PixelFormat::RGBA8888, input.height(), input.width(), out("out_04_rotate90.png"));

    RgaOperation rot180 {};
    rot180.op = RgaOp::Copy;
    rot180.rotation = RgaRotation::Rotate180;
    ok &= runRgaCase(rga, allocator, src.frame, rot180,
                     PixelFormat::RGBA8888, input.width(), input.height(), out("out_05_rotate180.png"));

    RgaOperation rot270 {};
    rot270.op = RgaOp::Copy;
    rot270.rotation = RgaRotation::Rotate270;
    ok &= runRgaCase(rga, allocator, src.frame, rot270,
                     PixelFormat::RGBA8888, input.height(), input.width(), out("out_06_rotate270.png"));

    RgaOperation mirrorH {};
    mirrorH.op = RgaOp::Copy;
    mirrorH.mirror = RgaMirror::Horizontal;
    ok &= runRgaCase(rga, allocator, src.frame, mirrorH,
                     PixelFormat::RGBA8888, input.width(), input.height(), out("out_07_mirror_h.png"));

    RgaOperation mirrorV {};
    mirrorV.op = RgaOp::Copy;
    mirrorV.mirror = RgaMirror::Vertical;
    ok &= runRgaCase(rga, allocator, src.frame, mirrorV,
                     PixelFormat::RGBA8888, input.width(), input.height(), out("out_08_mirror_v.png"));

    RgaOperation mirrorBoth {};
    mirrorBoth.op = RgaOp::Copy;
    mirrorBoth.mirror = RgaMirror::Both;
    ok &= runRgaCase(rga, allocator, src.frame, mirrorBoth,
                     PixelFormat::RGBA8888, input.width(), input.height(), out("out_09_mirror_both.png"));

    ok &= runConvertRoundTrip(rga, allocator, src.frame, out("out_10_convert_rgba_yuyv_rgba.png"));

    RgaOperation cropRotate {};
    cropRotate.op = RgaOp::Copy;
    cropRotate.crop = RgaRect {input.width() / 6, input.height() / 6, (input.width() / 2) & ~1, (input.height() / 2) & ~1};
    cropRotate.rotation = RgaRotation::Rotate90;
    ok &= runRgaCase(rga, allocator, src.frame, cropRotate,
                     PixelFormat::RGBA8888, cropRotate.crop.height, cropRotate.crop.width, out("out_11_combo_crop_rotate.png"));

    RgaOperation cropMirror {};
    cropMirror.op = RgaOp::Copy;
    cropMirror.crop = cropRotate.crop;
    cropMirror.mirror = RgaMirror::Horizontal;
    ok &= runRgaCase(rga, allocator, src.frame, cropMirror,
                     PixelFormat::RGBA8888, cropMirror.crop.width, cropMirror.crop.height, out("out_12_combo_crop_mirror.png"));

    if (!ok) {
        std::cerr << "some RGA cases failed\n";
        return 2;
    }

    std::cout << "all RGA cases passed, PNG files are in: " << exeDir << "\n";
    return 0;
}
