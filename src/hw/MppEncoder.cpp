#include "hw/MppEncoder.hpp"

#include "Log.hpp"
#include "hw/rkmpp_c/mpp_simple.h"

#include <cstring>
#include <sstream>
#include <utility>

namespace {

MppCodingType toMppCoding(MppCodec codec)
{
    switch (codec) {
    case MppCodec::H264:
        return MPP_VIDEO_CodingAVC;
    case MppCodec::H265:
        return MPP_VIDEO_CodingHEVC;
    case MppCodec::MJPEG:
        break;
    }
    return MPP_VIDEO_CodingUnused;
}

MppFrameFormat toMppFrameFormat(PixelFormat format)
{
    switch (format) {
    case PixelFormat::NV12:
        return MPP_FMT_YUV420SP;
    case PixelFormat::YUV420P:
        return MPP_FMT_YUV420P;
    case PixelFormat::YUYV:
        return MPP_FMT_YUV422_YUYV;
    case PixelFormat::RGBA8888:
        return MPP_FMT_RGBA8888;
    case PixelFormat::Unknown:
    case PixelFormat::Auto:
    case PixelFormat::MJPEG:
        break;
    }
    return MPP_FMT_BUTT;
}

const char* codecName(MppCodec codec)
{
    switch (codec) {
    case MppCodec::MJPEG:
        return "MJPEG";
    case MppCodec::H264:
        return "H264";
    case MppCodec::H265:
        return "H265";
    }
    return "Unknown";
}

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

        LOG_INFO("MppEncoder", "初始化 MPP 编码器 codec=" << codecName(cfg.codec)
                                 << " input=NV12 " << cfg.width << "x" << cfg.height
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
        headerWritten = false;
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
        headerWritten = false;
    }

    bool ensureHeaderWritten()
    {
        if (headerWritten)
            return true;

        callbackOk = true;
        if (rk_mpp_encoder_write_header(&encoder) != 0) {
            setError("rk_mpp_encoder_write_header 失败");
            return false;
        }

        headerWritten = true;
        return checkCallbackOk();
    }

    bool sendFrame(const VideoFrame& frame, bool eos)
    {
        if (!initialized) {
            setError("MppEncoder 尚未初始化");
            return false;
        }
        if (!validateFrame(frame))
            return false;
        if (!ensureHeaderWritten())
            return false;

        callbackOk = true;
        if (rk_mpp_encoder_send_frame(&encoder, frame.dmaFd, eos ? 1 : 0) != 0) {
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
        if (toMppCoding(cfg.codec) == MPP_VIDEO_CodingUnused ||
            toMppFrameFormat(cfg.inputFormat) == MPP_FMT_BUTT) {
            setError("MppEncoder 配置无法映射到 MPP");
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
        if (frame.format != PixelFormat::NV12) {
            setError("MppEncoder 输入帧必须是 NV12");
            return false;
        }
        if (frame.width != config.width || frame.height != config.height ||
            videoFrameEffectiveStride(frame) != (config.stride > 0 ? config.stride : config.width) ||
            videoFrameEffectiveHeightStride(frame) != (config.heightStride > 0 ? config.heightStride : config.height)) {
            std::ostringstream oss;
            oss << "MppEncoder 输入帧几何和初始化配置不一致: frame="
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
        packet.isHeader = isHeader != 0;
        packet.isKeyFrame = isIntra != 0;
        packet.eos = eos != 0;

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
    bool headerWritten = false;
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
