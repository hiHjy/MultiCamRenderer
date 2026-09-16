#pragma once

#include "VideoFrame.hpp"
#include "MppTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct EncodedPacket {
    const uint8_t* data = nullptr;
    size_t size = 0;
    MppCodec codec = MppCodec::H264;
    // 与输入 VideoFrame 对应的源时间戳。MPP header 不对应图像，值为 0。
    uint64_t timestampUs = 0;
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
    // MPP 的 prep 配置。编码器无法从 dmaFd 推断图像 layout，因此 init() 前必须填好。
    // IpcApp 不直接填写它们；RtspPublishSink 收到首帧后用真实 VideoFrame 补齐。
    int width = 0;
    int height = 0;
    int stride = 0;
    int heightStride = 0;
    PixelFormat inputFormat = PixelFormat::NV12;
    // 编码策略帧率，不是每帧显示时间。它用于 MPP CBR/VBR 的码率控制模型和默认 GOP；
    // 每帧的实际播放时间仍由 VideoFrame::timestampUs 传入。
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

    // config 必须包含完整的 MPP prep layout。MPP init 后不能逐帧换宽高/stride/格式；
    // 后续 sendFrame() 会校验输入保持与 config 一致，发生变化时由上层 deinit 后重建。
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
