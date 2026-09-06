#pragma once

#include "VideoCodec.hpp"

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
