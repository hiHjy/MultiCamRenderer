#include "hw/MppDecoder.hpp"

#include "Log.hpp"
#include "hw/rkmpp_c/mpp_advance.h"
#include "hw/rkmpp_c/mpp_simple.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

namespace {

MppCodingType toMppCoding(MppCodec codec)
{
    switch (codec) {
    case MppCodec::MJPEG:
        return MPP_VIDEO_CodingMJPEG;
    case MppCodec::H264:
        return MPP_VIDEO_CodingAVC;
    case MppCodec::H265:
        return MPP_VIDEO_CodingHEVC;
    }
    return MPP_VIDEO_CodingUnused;
}

PixelFormat fromMppFrameFormat(RK_U32 format)
{
    switch (format & MPP_FRAME_FMT_MASK) {
    case MPP_FMT_YUV420SP:
        return PixelFormat::NV12;
    case MPP_FMT_YUV420P:
        return PixelFormat::YUV420P;
    case MPP_FMT_YUV422_YUYV:
        return PixelFormat::YUYV;
    case MPP_FMT_RGBA8888:
        return PixelFormat::RGBA8888;
    default:
        return PixelFormat::Unknown;
    }
}

std::string codecName(MppCodec codec)
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

} // namespace

struct MppDecoder::Impl {
    Impl()
    {
        resetCStructs();
    }

    ~Impl()
    {
        deinit();
    }

    bool init(MppCodec codec)
    {
        if (initialized && currentCodec == codec) {
            lastError.clear();
            return true;
        }

        deinit();
        resetCStructs();
        currentCodec = codec;

        LOG_INFO("MppDecoder", "初始化 MPP 解码器 codec=" << codecName(codec));

        const MppCodingType coding = toMppCoding(codec);
        if (coding == MPP_VIDEO_CodingUnused) {
            setError("不支持的 MPP codec");
            return false;
        }

        if (codec == MppCodec::MJPEG) {
            if (rk_mpp_decoder_advance_init(&mjpegDecoder, coding) != 0) {
                setError("rk_mpp_decoder_advance_init 失败 codec=" + codecName(codec));
                return false;
            }
        } else {
            if (rk_mpp_decoder_init(&streamDecoder, coding, nullptr) != 0) {
                setError("rk_mpp_decoder_init 失败 codec=" + codecName(codec));
                return false;
            }
            rk_mpp_decoder_set_frame_callback(&streamDecoder, &Impl::streamFrameCallback, this);
        }

        initialized = true;
        lastError.clear();
        return true;
    }

    void deinit()
    {
        if (!initialized)
            return;

        if (currentCodec == MppCodec::MJPEG) {
            rk_mpp_decoder_advance_deinit(&mjpegDecoder);
        } else {
            rk_mpp_decoder_deinit(&streamDecoder);
        }

        initialized = false;
        callbackOk = true;
        resetCStructs();
    }

    bool decodeMjpeg(const VideoFrame& input, VideoFrame& output)
    {
        if (input.format != PixelFormat::MJPEG) {
            setError("decodeMjpeg 只接受 MJPEG 输入");
            return false;
        }
        if (input.dmaFd < 0 || input.bytesUsed == 0) {
            setError("MJPEG 输入 dmaFd/bytesUsed 无效");
            return false;
        }
        if (input.capacity < input.bytesUsed) {
            std::ostringstream oss;
            oss << "MJPEG 输入 capacity 小于 bytesUsed: capacity=" << input.capacity
                << " bytesUsed=" << input.bytesUsed;
            setError(oss.str());
            return false;
        }

        if (!initialized || currentCodec != MppCodec::MJPEG) {
            if (!init(MppCodec::MJPEG))
                return false;
        }

        const int width = output.width;
        const int height = output.height;
        const int stride = output.stride;
        const int heightStride = output.heightStride;
        const size_t requiredSize = videoFrameBufferSizeFor(PixelFormat::NV12,
                                                            stride,
                                                            heightStride,
                                                            VideoBufferSizeMode::MppDecoderOutput);

        if (width <= 0 || height <= 0 || stride <= 0 || heightStride <= 0 || requiredSize == 0) {
            setError("MJPEG 输出 VideoFrame layout 无效，调用方需要先填写宽高和 stride");
            return false;
        }

        if (output.dmaFd < 0 || output.capacity < requiredSize) {
            std::ostringstream oss;
            oss << "MJPEG 输出 buffer 无效或容量不足: fd=" << output.dmaFd
                << " capacity=" << output.capacity
                << " required=" << requiredSize;
            setError(oss.str());
            return false;
        }

        const RkMppInputPacket inputPacket {
            input.dmaFd,
            input.capacity,
            input.bytesUsed,
        };
        const RkMppOutputFrame outputFrame {
            output.dmaFd,
            output.capacity,
            width,
            height,
            stride,
            heightStride,
        };
        const int ret = rk_mpp_decoder_advance_do_task(&mjpegDecoder, &inputPacket, &outputFrame);
        if (ret != 0) {
            setError("rk_mpp_decoder_advance_do_task 失败");
            return false;
        }

        output.width = width;
        output.height = height;
        output.stride = stride;
        output.heightStride = heightStride;
        output.format = PixelFormat::NV12;
        output.nativeFormat = MPP_FMT_YUV420SP;
        output.bytesUsed = requiredSize;
        output.timestampUs = input.timestampUs;
        output.sequence = input.sequence;

        lastError.clear();
        return true;
    }

    bool sendPacket(const VideoFrame& packet, bool eos)
    {
        if (currentCodec != MppCodec::H264 && currentCodec != MppCodec::H265) {
            setError("sendPacket 只适用于 H264/H265，请先 init(H264/H265)");
            return false;
        }
        if (!initialized && !init(currentCodec))
            return false;
        if (packet.bytesUsed > 0 && packet.va == nullptr) {
            setError("H264/H265 packet 需要有效 va");
            return false;
        }

        callbackOk = true;
        lastError.clear();
        const int ret = rk_mpp_decoder_send_data_with_pts(&streamDecoder,
                                                          static_cast<const uint8_t*>(packet.va),
                                                          packet.bytesUsed,
                                                          eos ? 1 : 0,
                                                          static_cast<RK_S64>(packet.timestampUs));
        if (ret < 0) {
            if (lastError.empty())
                setError("rk_mpp_decoder_send_data_with_pts 失败");
            return false;
        }
        if (!callbackOk) {
            if (lastError.empty())
                setError("MppDecoder frame callback 返回失败");
            return false;
        }

        return true;
    }

    void setFrameCallback(FrameCallback cb)
    {
        callback = std::move(cb);
    }

    const std::string& error() const
    {
        return lastError;
    }

private:
    static void streamFrameCallback(const uint8_t* data,
                                    size_t size,
                                    int fd,
                                    RK_U32 width,
                                    RK_U32 height,
                                    RK_U32 hStride,
                                    RK_U32 vStride,
                                    RK_U32 fmt,
                                    RK_S64 ptsUs,
                                    void* userdata)
    {
        auto* self = static_cast<Impl*>(userdata);
        if (self == nullptr || !self->callback)
            return;

        VideoFrame frame {};
        frame.dmaFd = fd;
        frame.va = const_cast<uint8_t*>(data);
        frame.capacity = size;
        frame.bytesUsed = size;
        frame.width = static_cast<int>(width);
        frame.height = static_cast<int>(height);
        frame.stride = static_cast<int>(hStride);
        frame.heightStride = static_cast<int>(vStride);
        frame.format = fromMppFrameFormat(fmt);
        frame.nativeFormat = fmt;
        frame.timestampUs = ptsUs >= 0 ? static_cast<uint64_t>(ptsUs) : 0;

        if (!self->callback(frame)) {
            self->callbackOk = false;
            self->setError("MppDecoder frame callback 返回失败");
        }
    }

    void resetCStructs()
    {
        std::memset(&streamDecoder, 0, sizeof(streamDecoder));
        std::memset(&mjpegDecoder, 0, sizeof(mjpegDecoder));
        for (int& fd : mjpegDecoder.dst_fd)
            fd = -1;
    }

    void setError(const std::string& message)
    {
        lastError = message;
    }

    MppCodec currentCodec = MppCodec::MJPEG;
    bool initialized = false;
    bool callbackOk = true;
    RkMppDecoder streamDecoder {};
    MppDecoderAdvance mjpegDecoder {};
    FrameCallback callback;
    std::string lastError;
};

MppDecoder::MppDecoder()
    : m_impl(std::make_unique<Impl>())
{
}

MppDecoder::~MppDecoder() = default;

bool MppDecoder::init(MppCodec codec)
{
    return m_impl->init(codec);
}

void MppDecoder::deinit()
{
    m_impl->deinit();
}

bool MppDecoder::decodeMjpeg(const VideoFrame& input, VideoFrame& output)
{
    return m_impl->decodeMjpeg(input, output);
}

bool MppDecoder::sendPacket(const VideoFrame& packet, bool eos)
{
    return m_impl->sendPacket(packet, eos);
}

void MppDecoder::setFrameCallback(FrameCallback callback)
{
    m_impl->setFrameCallback(std::move(callback));
}

const std::string& MppDecoder::lastError() const
{
    return m_impl->error();
}
