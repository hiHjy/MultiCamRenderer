#include "Live555RtspClient.hh"
#include "MppTypes.hpp"
#include "VideoFrame.hpp"
#include "hw/MppDecoder.hpp"
#include <chrono>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <pthread.h>

// 这是 RTSP -> MPP 解码学习 demo 的空白起点。
// 你在这里创建 Live555RtspClient、注册 NALU 回调，并将 Annex-B NALU 送给
// MppDecoder::sendPacket()；需要记住回调中的 NALU 内存只在回调期间有效。

int main() {
	// 在创建 live555 工作线程前屏蔽信号，由下面的 sigwait() 同步接收。
	// 不在 signal handler 内直接 stop，避免异步上下文触碰 C++/live555 对象。
	sigset_t stopSignals;
	sigemptyset(&stopSignals);
	sigaddset(&stopSignals, SIGINT);
	sigaddset(&stopSignals, SIGTERM);
	if (pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr) != 0) {
		std::cerr << "屏蔽退出信号失败" << std::endl;
		return 1;
	}
	MppDecoder dec{};
	dec.init(MppCodec::H264);
	dec.setFrameCallback([](const VideoFrame &frame) -> bool {
		static auto statisticsBegin = std::chrono::steady_clock::now();
		static size_t frameCount = 0;
		++frameCount;

		const auto now = std::chrono::steady_clock::now();
		const std::chrono::duration<double> elapsed = now - statisticsBegin;
		if (elapsed.count() >= 1.0) {
			std::cout << "decode fps=" << frameCount / elapsed.count()
					  << " layout=" << frame.width << "x" << frame.height
					  << " stride=" << frame.stride << "x" << frame.heightStride
					  << " capacity=" << frame.capacity << '\n';
			statisticsBegin = now;
			frameCount = 0;
		}
		return true;
	});
	Live555RtspClient client{};
	if (!client.start("rtsp://192.168.1.5:8554/live",
					  [&dec](VideoCodec codec, uint8_t *data, size_t size, uint64_t timestampUs) {
						  static auto statisticsBegin = std::chrono::steady_clock::now();
						  static size_t naluCount = 0;
						  static size_t totalBytes = 0;
						  ++naluCount;
						  totalBytes += size;

						  const auto now = std::chrono::steady_clock::now();
						  const std::chrono::duration<double> elapsed = now - statisticsBegin;
						  if (elapsed.count() >= 1.0) {
							  std::cout << "input codec=" << (codec == VideoCodec::H264 ? "H264" : "H265")
										<< " nalu/s=" << naluCount / elapsed.count()
										<< " bitrate=" << totalBytes * 8.0 / elapsed.count() / 1000.0 << " Kbps\n";
							  statisticsBegin = now;
							  naluCount = 0;
							  totalBytes = 0;
						  }

						  VideoFrame packet{};
						  packet.va = data;
						  packet.bytesUsed = size;
						  packet.capacity = size;
						  packet.timestampUs = timestampUs;
						  dec.sendPacket(packet);
						  // 后续在这里将 data/size 封装成 VideoFrame，送 MppDecoder::sendPacket()。
						  // data 仅在当前回调中有效；要异步处理时必须复制。
						  (void)data;
					  })) {
		std::cerr << "RTSP 启动失败: " << client.lastError() << std::endl;
		return 1;
	}

	std::cout << "正在拉流，按 Ctrl+C 停止" << std::endl;

	int receivedSignal = 0;
	if (sigwait(&stopSignals, &receivedSignal) != 0) {
		std::cerr << "等待退出信号失败" << std::endl;
		client.stop();
		return 1;
	}

	std::cout << "收到退出信号 " << receivedSignal << "，停止 RTSP 客户端" << std::endl;
	client.stop();
	return 0;
}
