#pragma once

#include "VideoFrame.hpp"
#include "hw/MppDecoder.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct EncodedPacket {
    const uint8_t* data = nullptr;
    size_t size = 0;
    MppCodec codec = MppCodec::H264;
    bool isHeader = false;
    bool isKeyFrame = false;
    bool eos = false;
};

enum class MppBitratePreset {
    Low,
    Medium,
    High,
    VeryHigh,
};

struct MppEncoderConfig {
    MppCodec codec = MppCodec::H264;
    int width = 0;
    int height = 0;
    int stride = 0;
    int heightStride = 0;
    PixelFormat inputFormat = PixelFormat::NV12;
    int fps = 30;
    // 当 bitrate <= 0 时按档位自动计算：
    //   base = width * height * fps / 8
    //   Low      = base * 2 / 3
    //   Medium   = base
    //   High     = base * 3 / 2
    //   VeryHigh = base * 2
    // H265 会在上述结果上再乘以 65%，用于体现同等主观画质下的码率优势。
    MppBitratePreset bitratePreset = MppBitratePreset::Medium;
    // 精确码率，单位 bit/s。大于 0 时优先使用该值，忽略 bitratePreset。
    int bitrate = 0;
    int gop = 0;
};

class MppEncoder {
public:
    using PacketCallback = std::function<bool(const EncodedPacket& packet)>;

    MppEncoder();
    ~MppEncoder();

    MppEncoder(const MppEncoder&) = delete;
    MppEncoder& operator=(const MppEncoder&) = delete;

    MppEncoder(MppEncoder&&) = delete;
    MppEncoder& operator=(MppEncoder&&) = delete;

    bool init(const MppEncoderConfig& config);
    void deinit();

    bool sendFrame(const VideoFrame& frame, bool eos = false);
    bool requestKeyFrame();
    void setPacketCallback(PacketCallback callback);

    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
