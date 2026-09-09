#pragma once

#include "VideoCodec.hpp"

#include <cstddef>
#include <cstdint>

// 一段压缩视频码流的非 owning 视图。它不管理 data 的生命周期；调用方必须保证 data
// 在消费者约定的期间内有效。RTSP/live555 回调通常只能在当前回调内直接使用，若需跨线程
// 排队，必须先复制到自己的 vector 等持有型容器。
struct CompressedPacket {
    VideoCodec codec = VideoCodec::H264;
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint64_t timestampUs = 0;
};
