#include "DmaAllocator.hpp"
#include "VideoFrame.hpp"
#include "hw/MppDecoder.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {

void printUsage(const char* argv0)
{
    std::cout
        << "用法:\n"
        << "  " << argv0 << " mjpeg input.mjpg output.nv12\n"
        << "  " << argv0 << " h264 input.h264 first_frame.nv12\n"
        << "  " << argv0 << " h265 input.h265 first_frame.nv12\n";
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

bool extractFirstJpeg(const std::vector<unsigned char>& input,
                      std::vector<unsigned char>& jpeg)
{
    auto soi = input.end();
    for (auto it = input.begin(); it + 1 != input.end(); ++it) {
        if (it[0] == 0xFF && it[1] == 0xD8) {
            soi = it;
            break;
        }
    }
    if (soi == input.end()) {
        return false;
    }

    for (auto it = soi + 2; it + 1 != input.end(); ++it) {
        if (it[0] == 0xFF && it[1] == 0xD9) {
            jpeg.assign(soi, it + 2);
            return true;
        }
    }

    return false;
}

bool isJpegSofMarker(unsigned char marker)
{
    switch (marker) {
    case 0xC0:
    case 0xC1:
    case 0xC2:
    case 0xC3:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCD:
    case 0xCE:
    case 0xCF:
        return true;
    default:
        return false;
    }
}

bool parseJpegSize(const unsigned char* data, size_t len, int& width, int& height)
{
    if (data == nullptr || len < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return false;

    size_t pos = 2;
    while (pos + 3 < len) {
        while (pos < len && data[pos] != 0xFF)
            ++pos;
        while (pos < len && data[pos] == 0xFF)
            ++pos;
        if (pos >= len)
            break;

        const unsigned char marker = data[pos++];
        if (marker == 0xD8 || marker == 0x01)
            continue;
        if (marker == 0xD9 || marker == 0xDA)
            break;
        if (pos + 1 >= len)
            break;

        const size_t segLen = (static_cast<size_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
        if (segLen < 2 || pos + segLen - 2 > len)
            break;

        if (isJpegSofMarker(marker)) {
            if (segLen < 7)
                return false;
            height = (static_cast<int>(data[pos + 1]) << 8) | data[pos + 2];
            width = (static_cast<int>(data[pos + 3]) << 8) | data[pos + 4];
            return width > 0 && height > 0;
        }

        pos += segLen - 2;
    }

    return false;
}

int alignUp(int value, int alignment)
{
    if (alignment <= 1)
        return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

bool writeAll(std::ofstream& file, const unsigned char* data, size_t size)
{
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(file);
}

bool dumpNv12(const std::string& path, const VideoFrame& frame)
{
    if (frame.dmaFd < 0 || frame.width <= 0 || frame.height <= 0 ||
        frame.stride < frame.width || frame.heightStride < frame.height) {
        std::cerr << "NV12 frame 参数无效，无法 dump\n";
        return false;
    }

    const size_t mapSize = frame.capacity > 0
        ? frame.capacity
        : static_cast<size_t>(frame.stride) * static_cast<size_t>(frame.heightStride) * 3 / 2;
    void* mapped = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, frame.dmaFd, 0);
    if (mapped == MAP_FAILED) {
        std::cerr << "mmap 输出 fd 失败\n";
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::cerr << "打开输出文件失败: " << path << "\n";
        munmap(mapped, mapSize);
        return false;
    }

    const auto* base = static_cast<const unsigned char*>(mapped);
    const unsigned char* yPlane = base;
    const unsigned char* uvPlane = base + static_cast<size_t>(frame.stride) *
                                              static_cast<size_t>(frame.heightStride);

    for (int row = 0; row < frame.height; ++row) {
        if (!writeAll(file, yPlane + static_cast<size_t>(row) * frame.stride, frame.width)) {
            munmap(mapped, mapSize);
            return false;
        }
    }

    for (int row = 0; row < frame.height / 2; ++row) {
        if (!writeAll(file, uvPlane + static_cast<size_t>(row) * frame.stride, frame.width)) {
            munmap(mapped, mapSize);
            return false;
        }
    }

    munmap(mapped, mapSize);
    std::cout << "已保存 NV12: " << path << " "
              << frame.width << "x" << frame.height
              << " stride=" << frame.stride
              << " heightStride=" << frame.heightStride << "\n";
    return true;
}

bool copyToDma(const std::vector<unsigned char>& data, DmaMemory& memory)
{
    if (!memory.valid() || memory.size() < data.size())
        return false;

    std::memcpy(memory.va(), data.data(), data.size());
    return true;
}

size_t findStartCode(const std::vector<unsigned char>& data, size_t from)
{
    for (size_t i = from; i + 3 <= data.size(); ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01)
                return i;
            if (i + 4 <= data.size() && data[i + 2] == 0x00 && data[i + 3] == 0x01)
                return i;
        }
    }
    return data.size();
}

size_t startCodeLength(const std::vector<unsigned char>& data, size_t pos)
{
    if (pos + 4 <= data.size() && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
        data[pos + 2] == 0x00 && data[pos + 3] == 0x01) {
        return 4;
    }
    return 3;
}

int annexBNalType(MppCodec codec, const std::vector<unsigned char>& data, size_t start)
{
    const size_t header = start + startCodeLength(data, start);
    if (header >= data.size())
        return -1;

    if (codec == MppCodec::H264)
        return data[header] & 0x1F;
    if (codec == MppCodec::H265)
        return (data[header] >> 1) & 0x3F;
    return -1;
}

bool isVideoNal(MppCodec codec, int nalType)
{
    if (codec == MppCodec::H264)
        return nalType == 1 || nalType == 5;
    if (codec == MppCodec::H265)
        return nalType >= 0 && nalType <= 31;
    return false;
}

std::vector<std::vector<unsigned char>> splitAnnexBAccessUnits(MppCodec codec,
                                                               const std::vector<unsigned char>& data)
{
    std::vector<std::vector<unsigned char>> units;
    size_t unitStart = data.size();
    bool unitHasVideo = false;

    size_t nalStart = findStartCode(data, 0);
    while (nalStart < data.size()) {
        const size_t nextNal = findStartCode(data, nalStart + startCodeLength(data, nalStart));
        const int nalType = annexBNalType(codec, data, nalStart);
        const bool videoNal = isVideoNal(codec, nalType);

        if (unitStart == data.size()) {
            unitStart = nalStart;
        } else if (videoNal && unitHasVideo) {
            units.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(unitStart),
                               data.begin() + static_cast<std::ptrdiff_t>(nalStart));
            unitStart = nalStart;
            unitHasVideo = false;
        }

        unitHasVideo = unitHasVideo || videoNal;
        nalStart = nextNal;
    }

    if (unitStart < data.size()) {
        units.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(unitStart), data.end());
    }

    return units;
}

bool runMjpeg(const std::string& inputPath, const std::string& outputPath)
{
    std::vector<unsigned char> fileData;
    if (!readFile(inputPath, fileData))
        return false;

    std::vector<unsigned char> jpegData;
    if (!extractFirstJpeg(fileData, jpegData)) {
        std::cerr << "输入文件里没有找到完整 JPEG 帧\n";
        return false;
    }

    DmaAllocator allocator;
    DmaMemory inputMemory;
    if (!allocator.allocate(jpegData.size(), inputMemory)) {
        std::cerr << "分配 MJPEG 输入 DMA 失败: " << allocator.lastError() << "\n";
        return false;
    }
    if (!copyToDma(jpegData, inputMemory)) {
        std::cerr << "复制 MJPEG 输入数据失败\n";
        return false;
    }

    VideoFrame input {};
    input.dmaFd = inputMemory.fd();
    input.va = inputMemory.va();
    input.capacity = inputMemory.size();
    input.bytesUsed = jpegData.size();
    input.format = PixelFormat::MJPEG;

    int width = 0;
    int height = 0;
    if (!parseJpegSize(jpegData.data(), jpegData.size(), width, height)) {
        std::cerr << "解析 MJPEG 尺寸失败\n";
        return false;
    }
    input.width = width;
    input.height = height;

    DmaMemory outputMemory;
    const size_t outputSize = static_cast<size_t>(alignUp(width, 16)) *
                              static_cast<size_t>(alignUp(height, 16)) * 2;
    if (!allocator.allocate(outputSize, outputMemory)) {
        std::cerr << "分配 MJPEG 输出 DMA 失败: " << allocator.lastError() << "\n";
        return false;
    }

    VideoFrame output {};
    output.dmaFd = outputMemory.fd();
    output.va = outputMemory.va();
    output.capacity = outputMemory.size();

    MppDecoder decoder;
    if (!decoder.decodeMjpeg(input, output)) {
        std::cerr << "MJPEG 解码失败: " << decoder.lastError() << "\n";
        return false;
    }

    return dumpNv12(outputPath, output);
}

MppCodec parseCodec(const std::string& text)
{
    if (text == "h264")
        return MppCodec::H264;
    if (text == "h265")
        return MppCodec::H265;
    return MppCodec::MJPEG;
}

bool runStream(const std::string& codecName,
               const std::string& inputPath,
               const std::string& outputPath)
{
    std::vector<unsigned char> fileData;
    if (!readFile(inputPath, fileData))
        return false;

    const MppCodec codec = parseCodec(codecName);
    std::vector<std::vector<unsigned char>> accessUnits = splitAnnexBAccessUnits(codec, fileData);
    if (accessUnits.empty()) {
        std::cerr << "输入文件没有找到 Annex-B access unit: " << inputPath << "\n";
        return false;
    }

    MppDecoder decoder;
    if (!decoder.init(codec)) {
        std::cerr << "初始化解码器失败: " << decoder.lastError() << "\n";
        return false;
    }

    bool dumped = false;
    decoder.setFrameCallback([&](const VideoFrame& frame) {
        std::cout << "收到解码帧: " << frame.width << "x" << frame.height
                  << " stride=" << frame.stride
                  << " heightStride=" << frame.heightStride
                  << " fd=" << frame.dmaFd << "\n";
        if (!dumped) {
            dumped = dumpNv12(outputPath, frame);
        }
        return true;
    });

    uint64_t pts = 0;
    for (const std::vector<unsigned char>& accessUnit : accessUnits) {
        VideoFrame packet {};
        packet.va = const_cast<unsigned char*>(accessUnit.data());
        packet.bytesUsed = accessUnit.size();
        packet.capacity = accessUnit.size();
        packet.timestampUs = pts;
        pts += 33000;

        if (!decoder.sendPacket(packet, false)) {
            std::cerr << "sendPacket 失败: " << decoder.lastError() << "\n";
            return false;
        }
    }

    VideoFrame eos {};
    eos.bytesUsed = 0;
    if (!decoder.sendPacket(eos, true)) {
        std::cerr << "发送 EOS 失败: " << decoder.lastError() << "\n";
        return false;
    }

    if (!dumped) {
        std::cerr << "没有收到可 dump 的解码帧\n";
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 4) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string codec = argv[1];
    const std::string input = argv[2];
    const std::string output = argv[3];

    if (codec == "mjpeg") {
        return runMjpeg(input, output) ? 0 : 1;
    }
    if (codec == "h264" || codec == "h265") {
        return runStream(codec, input, output) ? 0 : 1;
    }

    printUsage(argv[0]);
    return 1;
}
