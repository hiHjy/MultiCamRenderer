#pragma once

#include "CompressedPacket.hpp"
#include "VideoFrame.hpp"
#include "MppTypes.hpp"

#include <functional>
#include <memory>
#include <string>

class MppDecoder {
  public:
	// H264/H265 输出是 MPP 临时帧视图，只在 callback 返回前有效。
	// 需要异步使用时，必须在 callback 内立即 RGA/copy 到自己的稳定 buffer。
	using FrameCallback = std::function<bool(const VideoFrame &frame)>;

	MppDecoder();
	~MppDecoder();

	MppDecoder(const MppDecoder &) = delete;
	MppDecoder &operator=(const MppDecoder &) = delete;

	MppDecoder(MppDecoder &&) = delete;
	MppDecoder &operator=(MppDecoder &&) = delete;

	bool init(MppCodec codec);
	void deinit();

	bool decodeMjpeg(const VideoFrame &input, VideoFrame &output);

    // H264/H265 流式入口接收顺序正确、带 Annex-B 起始码的压缩数据。CompressedPacket
    // 只借用 data，调用返回前调用方必须保持其有效。
	// 当前底层为 H264/H265 开启了 MPP split_parse，因此 RTSP/live555 可以一整个
	// NALU 一次调用；也可以传上层已经组好的 access unit。不能按任意字节位置切断。
	// 解码器不负责 RTP 重组、网络重连，也不配置宽高/fps/stride；这些信息由码流和
	// MPP info_change 决定。
    bool sendPacket(const CompressedPacket &packet, bool eos = false);
	void setFrameCallback(FrameCallback callback);

	const std::string &lastError() const;

  private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
