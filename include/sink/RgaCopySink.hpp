#pragma once

#include "Sink.hpp"
#include "DmaBufferPool.hpp"
#include "Log.hpp"
#include "hw/RgaEngine.hpp"

#include <chrono>
#include <iostream>
#include <string>

class RgaCopySink : public Sink {
public:
	RgaCopySink()
	{
		m_pool.init(4,
		            videoFrameBufferSizeFor(PixelFormat::YUYV,
		                                    1280,
		                                    720,
		                                    0,
		                                    VideoBufferSizeMode::Payload));
	}
    ~RgaCopySink() override = default;

    void onFrame(FramePacket packet) override
	{
		const VideoFrame& frame = packet.frame;
		if (m_pool.state() == DmaBufferPool::PoolState::Busy || m_pool.state() == DmaBufferPool::PoolState::Uninitialized) {
			LOG_WARN("RgaCopySink", "pool 不可用 state=" << DmaBufferPool::stateName(m_pool.state()));
			return;
		}

		VideoFrame* dstFrame = m_pool.acquireFrame();
		if (dstFrame == nullptr) {
			LOG_WARN("RgaCopySink", "pool acquire 失败: " << m_pool.lastError());
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
#if LOG_ACTIVE_LEVEL <= LOG_LEVEL_INFO || LOG_ACTIVE_LEVEL <= LOG_LEVEL_ERROR
		const auto begin = std::chrono::steady_clock::now();
#endif
		if (!m_rga.copy(frame, *dstFrame)) {
#if LOG_ACTIVE_LEVEL <= LOG_LEVEL_ERROR
			const auto end = std::chrono::steady_clock::now();
			const auto costUs = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
			LOG_ERROR("RgaCopySink", "rga copy failed cost_us=" << costUs
			          << " error=" << m_rga.lastError());
#endif
			m_pool.releaseFrame(dstFrame);
			return;
		}

#if LOG_ACTIVE_LEVEL <= LOG_LEVEL_INFO
		const auto end = std::chrono::steady_clock::now();
		const auto costUs = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
		LOG_INFO("RgaCopySink", "rga copy success cost_us=" << costUs
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
		          << "}");
#endif

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
