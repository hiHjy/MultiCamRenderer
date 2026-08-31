#include "DmaAllocator.hpp"
#include "VideoFrame.hpp"
#include "hw/MppEncoder.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <linux/dma-buf.h>
#include <string>
#include <sys/ioctl.h>
#include <vector>

namespace {

void printUsage(const char* argv0)
{
    std::cout
        << "用法:\n"
        << "  " << argv0 << " h264 input.nv12 output.h264 width height [stride] [heightStride] [fps] [bitrate|low|medium|high|veryhigh] [frameCount] [requestKeyFrameIndex]\n"
        << "  " << argv0 << " h265 input.nv12 output.h265 width height [stride] [heightStride] [fps] [bitrate|low|medium|high|veryhigh] [frameCount] [requestKeyFrameIndex]\n";
}

int parseInt(const char* text, int fallback)
{
    if (text == nullptr)
        return fallback;

    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text)
        return fallback;
    return static_cast<int>(value);
}

MppCodec parseCodec(const std::string& text)
{
    if (text == "h265")
        return MppCodec::H265;
    return MppCodec::H264;
}

MppBitratePreset parseBitratePreset(const std::string& text, MppBitratePreset fallback)
{
    if (text == "low")
        return MppBitratePreset::Low;
    if (text == "medium" || text == "normal")
        return MppBitratePreset::Medium;
    if (text == "high")
        return MppBitratePreset::High;
    if (text == "veryhigh" || text == "ultra")
        return MppBitratePreset::VeryHigh;
    return fallback;
}

bool parseBitrateArg(const char* text, int& bitrate, MppBitratePreset& preset)
{
    bitrate = 0;
    preset = MppBitratePreset::Medium;
    if (text == nullptr)
        return true;

    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end != text && *end == '\0') {
        bitrate = static_cast<int>(value);
        return bitrate > 0;
    }

    const std::string presetText(text);
    const MppBitratePreset parsedPreset = parseBitratePreset(presetText, preset);
    if (parsedPreset == preset && presetText != "medium" && presetText != "normal")
        return false;

    preset = parsedPreset;
    return true;
}

bool readFile(const std::string& path, std::vector<unsigned char>& data)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "打开输入文件失败: " << path << "\n";
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        std::cerr << "输入文件为空: " << path << "\n";
        return false;
    }
    file.seekg(0, std::ios::beg);

    data.resize(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        std::cerr << "读取输入文件失败: " << path << "\n";
        return false;
    }

    return true;
}

size_t nv12PayloadSize(int stride, int heightStride)
{
    return static_cast<size_t>(stride) * static_cast<size_t>(heightStride) * 3 / 2;
}

void dmabufSync(int fd, unsigned long flags)
{
    if (fd < 0)
        return;

    dma_buf_sync sync {};
    sync.flags = flags;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 6) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string codecText = argv[1];
    if (codecText != "h264" && codecText != "h265") {
        printUsage(argv[0]);
        return 1;
    }

    const std::string inputPath = argv[2];
    const std::string outputPath = argv[3];
    const int width = parseInt(argv[4], 0);
    const int height = parseInt(argv[5], 0);
    const int stride = argc > 6 ? parseInt(argv[6], width) : width;
    const int heightStride = argc > 7 ? parseInt(argv[7], height) : height;
    const int fps = argc > 8 ? parseInt(argv[8], 30) : 30;
    const int requestedFrameCount = argc > 10 ? parseInt(argv[10], -1) : -1;
    const int requestKeyFrameIndex = argc > 11 ? parseInt(argv[11], -1) : -1;
    int bitrate = 0;
    MppBitratePreset bitratePreset = MppBitratePreset::Medium;

    if (width <= 0 || height <= 0 || stride < width || heightStride < height) {
        std::cerr << "宽高/stride 参数无效\n";
        return 1;
    }
    if (!parseBitrateArg(argc > 9 ? argv[9] : nullptr, bitrate, bitratePreset)) {
        std::cerr << "码率参数无效，请传正整数或 low/medium/high/veryhigh\n";
        return 1;
    }

    std::vector<unsigned char> fileData;
    if (!readFile(inputPath, fileData))
        return 1;

    const size_t frameSize = nv12PayloadSize(stride, heightStride);
    if (fileData.size() < frameSize) {
        std::cerr << "输入 NV12 数据不足: file=" << fileData.size()
                  << " required=" << frameSize << "\n";
        return 1;
    }
    const size_t availableFrames = fileData.size() / frameSize;
    const int frameCount = requestedFrameCount > 0
        ? std::min<int>(requestedFrameCount, static_cast<int>(availableFrames))
        : static_cast<int>(availableFrames);
    if (frameCount <= 0) {
        std::cerr << "没有可编码的完整 NV12 帧\n";
        return 1;
    }

    DmaAllocator allocator;
    DmaMemory inputMemory;
    if (!allocator.allocate(frameSize, inputMemory)) {
        std::cerr << "分配输入 DMA 失败: " << allocator.lastError() << "\n";
        return 1;
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "打开输出文件失败: " << outputPath << "\n";
        return 1;
    }

    MppEncoder encoder;
    encoder.setPacketCallback([&](const EncodedPacket& packet) {
        std::cout << "收到编码包: size=" << packet.size
                  << " header=" << packet.isHeader
                  << " keyFrame=" << packet.isKeyFrame
                  << " eos=" << packet.eos << "\n";
        if (packet.data != nullptr && packet.size > 0)
            output.write(reinterpret_cast<const char*>(packet.data),
                         static_cast<std::streamsize>(packet.size));
        return static_cast<bool>(output);
    });

    MppEncoderConfig config {};
    config.codec = parseCodec(codecText);
    config.width = width;
    config.height = height;
    config.stride = stride;
    config.heightStride = heightStride;
    config.inputFormat = PixelFormat::NV12;
    config.fps = fps;
    config.bitratePreset = bitratePreset;
    config.bitrate = bitrate;
    config.gop = 15;

    if (!encoder.init(config)) {
        std::cerr << "初始化编码器失败: " << encoder.lastError() << "\n";
        return 1;
    }

    VideoFrame frame {};
    frame.dmaFd = inputMemory.fd();
    frame.va = inputMemory.va();
    frame.capacity = inputMemory.size();
    frame.bytesUsed = frameSize;
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.heightStride = heightStride;
    frame.format = PixelFormat::NV12;

    for (int i = 0; i < frameCount; ++i) {
        if (i == requestKeyFrameIndex) {
            std::cout << "请求下一帧关键帧: frameIndex=" << i << "\n";
            if (!encoder.requestKeyFrame()) {
                std::cerr << "请求关键帧失败: " << encoder.lastError() << "\n";
                return 1;
            }
        }

        const size_t offset = static_cast<size_t>(i) * frameSize;
        dmabufSync(inputMemory.fd(), DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        std::memcpy(inputMemory.va(), fileData.data() + offset, frameSize);
        dmabufSync(inputMemory.fd(), DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

        frame.sequence = static_cast<uint64_t>(i);
        frame.timestampUs = static_cast<uint64_t>(i) * 1000000ULL / static_cast<uint64_t>(fps);
        if (!encoder.sendFrame(frame, i == frameCount - 1)) {
            std::cerr << "编码帧失败 index=" << i << ": " << encoder.lastError() << "\n";
            return 1;
        }
    }

    output.close();
    std::cout << "已保存编码输出: " << outputPath << "\n";
    return 0;
}
