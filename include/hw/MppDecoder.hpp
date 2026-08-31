#pragma once

#include "VideoFrame.hpp"
#include "hw/MppTypes.hpp"

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

	// H264/H265 流式入口只接收上层已经切好的压缩包。
	// 对 Annex-B 来说，packet 应该是一帧 access unit，而不是随便按字节数切出来的一段。
	// 解码器不负责从 RTSP/RTP/文件流里拆包、组帧，也不配置宽高/fps/stride；
	// 这些信息由码流和 MPP info_change 决定。
	bool sendPacket(const VideoFrame &packet, bool eos = false);
	void setFrameCallback(FrameCallback callback);

	const std::string &lastError() const;

  private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
