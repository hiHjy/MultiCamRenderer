#include "MppTypes.hpp"

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

const char* mppCodecName(MppCodec codec)
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
