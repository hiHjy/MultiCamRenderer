#pragma once

#include "Consumer.hpp"
#include "DmaBufferPool.hpp"
#include "hw/RgaEngine.hpp"

#include <chrono>
#include <iostream>
#include <string>

class RgaCopyConsumer : public Consumer {
public:
    RgaCopyConsumer()
	{
		m_pool.init(4, 1280 * 720 * 2);
		
	}
    ~RgaCopyConsumer() override = default;

    void onFrame(FramePacket packet) override
	{
		const VideoFrame& frame = packet.frame;
		if (m_pool.state() == DmaBufferPool::PoolState::Busy || m_pool.state() == DmaBufferPool::PoolState::Uninitialized) {
			std::cout << "pool 不可用" << std::endl;
			return;
		}

		VideoFrame* dstFrame = m_pool.acquireFrame();
		if (dstFrame == nullptr) {
			std::cout << "pool acquire 失败: " << m_pool.lastError() << std::endl;
			return;
		}

		dstFrame->streamId = frame.streamId;
		dstFrame->width = frame.width;
		dstFrame->height = frame.height;
		dstFrame->stride = frame.stride;
		dstFrame->heightStride = frame.heightStride;
		dstFrame->format = frame.format;
		dstFrame->nativeFormat = frame.nativeFormat;

		// 测试版直接同步 RGA copy。
		// 真正的异步消费者应该在 copy 成功后把 dstFrame 放进自己的 ready 队列，
		// 由 worker 线程处理完再 releaseFrame()，不要在 onFrame 里做慢操作。
		const auto begin = std::chrono::steady_clock::now();
		if (!m_rga.copy(frame, *dstFrame)) {
			const auto end = std::chrono::steady_clock::now();
			const auto costUs = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
			std::cout << "rga error: " << m_rga.lastError() << std::endl;
			std::cout << "rga copy cost_us=" << costUs << std::endl;
			m_pool.releaseFrame(dstFrame);
			return;
		}
		const auto end = std::chrono::steady_clock::now();
		const auto costUs = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();

		std::cout << "rga success cost_us=" << costUs
		          << " src{stream=" << frame.streamId
		          << " seq=" << frame.sequence
		          << " ts=" << frame.timestampUs
		          << " " << frame.width << "x" << frame.height
		          << " stride=" << frame.stride
		          << " hstride=" << frame.heightStride
		          << " fmt=" << static_cast<int>(frame.format)
		          << " fd=" << frame.dmaFd
		          << " bytes=" << frame.bytesUsed << "/" << frame.capacity
		          << " idx=" << frame.bufferIndex
		          << "} dst{stream=" << dstFrame->streamId
		          << " seq=" << dstFrame->sequence
		          << " ts=" << dstFrame->timestampUs
		          << " " << dstFrame->width << "x" << dstFrame->height
		          << " stride=" << dstFrame->stride
		          << " hstride=" << dstFrame->heightStride
		          << " fmt=" << static_cast<int>(dstFrame->format)
		          << " fd=" << dstFrame->dmaFd
		          << " bytes=" << dstFrame->bytesUsed << "/" << dstFrame->capacity
		          << " idx=" << dstFrame->bufferIndex
		          << "}" << std::endl;
		
		// 当前 demo 假设 RGA copy 后立刻用完，所以马上归还。
		// 后续接 DRM/AI/编码时，应由对应消费者的 worker 线程归还。
		m_pool.releaseFrame(dstFrame);
		
	}

    const std::string& lastError() const;

private:
    RgaEngine m_rga;
    DmaBufferPool m_pool;
    std::string m_lastError;
};
