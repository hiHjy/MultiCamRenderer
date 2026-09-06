#pragma once

#include <cstdint>

// 与传输协议、解码器实现无关的视频压缩编码标识。
// H264/H265 等传输层类型先转换为该通用类型，再交给具体硬件模块处理。
enum class VideoCodec : uint8_t {
    H264,
    H265,
};
