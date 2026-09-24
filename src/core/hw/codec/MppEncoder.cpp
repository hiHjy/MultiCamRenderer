#include "MppEncoder.hpp"

#include "Log.hpp"
#include "mpp_simple.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <utility>

#ifndef MCR_MPP_ENCODER_DEBUG_IDR
#define MCR_MPP_ENCODER_DEBUG_IDR 0
#endif

namespace {

#if MCR_MPP_ENCODER_DEBUG_IDR
// MPP 输出为 Annex-B。只在调试编译中扫描整组 packet，避免生产编码回调增加任何工作。
// H264 IDR 的 nal_unit_type 为 5；H265 的 IDR_W_RADL / IDR_N_LP 为 19 / 20。
bool containsIdrNalu(MppCodec codec, const uint8_t* data, size_t size)
{
    if (data == nullptr || size < 5) {
        return false;
    }

    for (size_t index = 0; index + 4 < size; ++index) {
        size_t headerOffset = 0;
        if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1) {
            headerOffset = index + 3;
        } else if (index + 4 < size && data[index] == 0 && data[index + 1] == 0
                   && data[index + 2] == 0 && data[index + 3] == 1) {
            headerOffset = index + 4;
        } else {
            continue;
        }

        if (headerOffset >= size) {
            break;
        }
        if (codec == MppCodec::H264 && (data[headerOffset] & 0x1F) == 5) {
            return true;
        }
        if (codec == MppCodec::H265 && ((data[headerOffset] >> 1) & 0x3F) >= 19
            && ((data[headerOffset] >> 1) & 0x3F) <= 20) {
            return true;
        }
    }
    return false;
}
#endif

const char* bitratePresetName(MppBitratePreset preset)
{
    switch (preset) {
    case MppBitratePreset::Low:
        return "Low";
    case MppBitratePreset::Medium:
        return "Medium";
    case MppBitratePreset::High:
        return "High";
    case MppBitratePreset::VeryHigh:
        return "VeryHigh";
    }
    return "Unknown";
}

int presetBitrate(const MppEncoderConfig& cfg, int fps)
{
    const int base = cfg.width * cfg.height * fps / 8;
    int bitrate = base;

    switch (cfg.bitratePreset) {
    case MppBitratePreset::Low:
        bitrate = base * 2 / 3;
        break;
    case MppBitratePreset::Medium:
        bitrate = base;
        break;
    case MppBitratePreset::High:
        bitrate = base * 3 / 2;
        break;
    case MppBitratePreset::VeryHigh:
        bitrate = base * 2;
        break;
    }

    if (cfg.codec == MppCodec::H265)
        bitrate = bitrate * 65 / 100;

    return bitrate > 0 ? bitrate : base;
}

} // namespace

struct MppEncoder::Impl {
    Impl()
    {
        std::memset(&encoder, 0, sizeof(encoder));
    }

    ~Impl()
    {
        deinit();
    }

    bool init(const MppEncoderConfig& cfg)
    {
        if (initialized)
            deinit();

        if (!validateConfig(cfg))
            return false;

        config = cfg;
        const int stride = cfg.stride > 0 ? cfg.stride : cfg.width;
        const int heightStride = cfg.heightStride > 0 ? cfg.heightStride : cfg.height;
        const int fps = cfg.fps > 0 ? cfg.fps : 30;
        const int bitrate = cfg.bitrate > 0 ? cfg.bitrate : presetBitrate(cfg, fps);
        const int gop = cfg.gop > 0 ? cfg.gop : fps;

        LOG_INFO("MppEncoder", "初始化 MPP 编码器 codec=" << mppCodecName(cfg.codec)
                                 << " input=NV12 "
                                 << cfg.width << "x" << cfg.height
                                 << " stride=" << stride << "x" << heightStride
                                 << " fps=" << fps
                                 << " bitratePreset=" << bitratePresetName(cfg.bitratePreset)
                                 << " bitrate=" << bitrate
                                 << " gop=" << gop);

        const int ret = rk_mpp_encoder_init(&encoder,
                                            toMppCoding(cfg.codec),
                                            static_cast<RK_U32>(cfg.width),
                                            static_cast<RK_U32>(cfg.height),
                                            static_cast<RK_U32>(stride),
                                            static_cast<RK_U32>(heightStride),
                                            toMppFrameFormat(cfg.inputFormat),
                                            fps,
                                            bitrate,
                                            gop,
                                            nullptr);
        if (ret != 0) {
            setError("rk_mpp_encoder_init 失败");
            return false;
        }

        rk_mpp_encoder_set_packet_callback(&encoder, &Impl::packetCallback, this);
        initialized = true;
        lastError.clear();
        return true;
    }

    void deinit()
    {
        if (!initialized)
            return;

        rk_mpp_encoder_deinit(&encoder);
        std::memset(&encoder, 0, sizeof(encoder));
        initialized = false;
        callbackOk = true;
    }

    bool sendFrame(const VideoFrame& frame, bool eos)
    {
        if (!initialized) {
            setError("MppEncoder 尚未初始化");
            return false;
        }
        if (!validateFrame(frame))
            return false;

        callbackOk = true;
        if (rk_mpp_encoder_send_frame(&encoder, frame.dmaFd, frame.timestampUs, eos ? 1 : 0) != 0) {
            setError("rk_mpp_encoder_send_frame 失败");
            return false;
        }

        return checkCallbackOk();
    }

    bool requestKeyFrame()
    {
        if (!initialized) {
            setError("MppEncoder 尚未初始化");
            return false;
        }

        if (rk_mpp_encoder_request_idr(&encoder) != 0) {
            setError("rk_mpp_encoder_request_idr 失败");
            return false;
        }

        lastError.clear();
        return true;
    }

    void setPacketCallback(PacketCallback cb)
    {
        callback = std::move(cb);
    }

    const std::string& error() const
    {
        return lastError;
    }

private:
    bool validateConfig(const MppEncoderConfig& cfg)
    {
        if (cfg.codec != MppCodec::H264 && cfg.codec != MppCodec::H265) {
            setError("MppEncoder 第一版只支持 H264/H265");
            return false;
        }
        if (toMppCoding(cfg.codec) == MPP_VIDEO_CodingUnused) {
            setError("MppEncoder 编码类型无法映射到 MPP");
            return false;
        }
        if (cfg.inputFormat != PixelFormat::NV12) {
            setError("MppEncoder 第一版只接受 NV12，其他格式请先用 RGA 转换");
            return false;
        }
        if (cfg.width <= 0 || cfg.height <= 0) {
            setError("MppEncoder 宽高必须大于 0");
            return false;
        }
        if (cfg.stride > 0 && cfg.stride < cfg.width) {
            setError("MppEncoder stride 不能小于 width");
            return false;
        }
        if (cfg.heightStride > 0 && cfg.heightStride < cfg.height) {
            setError("MppEncoder heightStride 不能小于 height");
            return false;
        }
        if (toMppFrameFormat(cfg.inputFormat) == MPP_FMT_BUTT) {
            setError("MppEncoder 输入格式无法映射到 MPP");
            return false;
        }
        return true;
    }

    bool validateFrame(const VideoFrame& frame)
    {
        if (frame.dmaFd < 0) {
            setError("MppEncoder 输入帧 dmaFd 无效");
            return false;
        }
        if (frame.format != config.inputFormat) {
            setError("MppEncoder 输入帧格式和初始化配置不一致");
            return false;
        }
        if (frame.width != config.width || frame.height != config.height ||
            videoFrameEffectiveStride(frame) != (config.stride > 0 ? config.stride : config.width) ||
            videoFrameEffectiveHeightStride(frame) != (config.heightStride > 0 ? config.heightStride : config.height)) {
            std::ostringstream oss;
            oss << "MppEncoder 输入帧 layout 和初始化配置不一致: frame="
                << frame.width << "x" << frame.height
                << " stride=" << videoFrameEffectiveStride(frame) << "x"
                << videoFrameEffectiveHeightStride(frame)
                << " config=" << config.width << "x" << config.height
                << " stride=" << (config.stride > 0 ? config.stride : config.width)
                << "x" << (config.heightStride > 0 ? config.heightStride : config.height);
            setError(oss.str());
            return false;
        }
        return true;
    }

    bool checkCallbackOk()
    {
        if (!callbackOk) {
            if (lastError.empty())
                setError("MppEncoder packet callback 返回失败");
            return false;
        }
        lastError.clear();
        return true;
    }

    static void packetCallback(const uint8_t* data,
                               size_t size,
                               uint64_t timestampUs,
                               int isHeader,
                               int isIntra,
                               int eos,
                               void* userdata)
    {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr || !self->callback)
            return;

        EncodedPacket packet {};
        packet.data = data;
        packet.size = size;
        packet.codec = self->config.codec;
        packet.timestampUs = timestampUs;
        packet.isHeader = isHeader != 0;
        packet.isKeyFrame = isIntra != 0;
        packet.eos = eos != 0;

#if MCR_MPP_ENCODER_DEBUG_IDR
        // 这组 packet 含参数集和 IDR 时才输出一行。直接写 stderr 并 flush，后台重定向
        // 到日志文件时也能立即采样；这段代码在宏关闭时完全不参与生产路径。
        if (containsIdrNalu(packet.codec, packet.data, packet.size)) {
            std::fprintf(stderr,
                         "[MppEncoder IDR debug] codec=%s bytes=%zu ptsUs=%llu mppIntra=%d\n",
                         mppCodecName(packet.codec), packet.size,
                         static_cast<unsigned long long>(packet.timestampUs), isIntra != 0 ? 1 : 0);
            std::fflush(stderr);
        }
#endif

        if (!self->callback(packet)) {
            self->callbackOk = false;
            self->setError("MppEncoder packet callback 返回失败");
        }
    }

    void setError(const std::string& message)
    {
        lastError = message;
    }

    RkMppEncoder encoder {};
    MppEncoderConfig config {};
    PacketCallback callback;
    bool initialized = false;
    bool callbackOk = true;
    std::string lastError;
};

MppEncoder::MppEncoder()
    : m_impl(std::make_unique<Impl>())
{
}

MppEncoder::~MppEncoder() = default;

bool MppEncoder::init(const MppEncoderConfig& config)
{
    return m_impl->init(config);
}

void MppEncoder::deinit()
{
    m_impl->deinit();
}

bool MppEncoder::sendFrame(const VideoFrame& frame, bool eos)
{
    return m_impl->sendFrame(frame, eos);
}

bool MppEncoder::requestKeyFrame()
{
    return m_impl->requestKeyFrame();
}

void MppEncoder::setPacketCallback(PacketCallback callback)
{
    m_impl->setPacketCallback(std::move(callback));
}

const std::string& MppEncoder::lastError() const
{
    return m_impl->error();
}
