#pragma once

#include "VideoCodec.hpp"
#include "VideoFrame.hpp"

extern "C" {
#include "mpp_frame.h"
#include "rk_mpi.h"
}

enum class MppCodec {
    MJPEG,
    H264,
    H265,
};

// 网络/文件等上游使用 VideoCodec；MPP 模块只接收 MppCodec。
// 编码转换集中在此处，避免每个调用点各自维护一份 switch。
constexpr MppCodec toMppCodec(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::H264:
        return MppCodec::H264;
    case VideoCodec::H265:
        return MppCodec::H265;
    }
    return MppCodec::H264;
}

// 以下函数集中维护项目类型与 Rockchip MPP 类型之间的固定映射。
// MppCodec::MJPEG 能映射为 MPP 编码类型，但是否支持该能力仍由具体模块决定：
// MppDecoder 支持 MJPEG，MppEncoder 当前只支持 H264/H265。
MppCodingType toMppCoding(MppCodec codec);
MppFrameFormat toMppFrameFormat(PixelFormat format);
PixelFormat fromMppFrameFormat(RK_U32 format);
const char* mppCodecName(MppCodec codec);
