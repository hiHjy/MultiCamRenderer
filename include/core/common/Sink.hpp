#pragma once

#include "VideoFrame.hpp"

class Sink {
public:
	virtual ~Sink() = default;

	// 帧回调：packet 按值传入，sink 会拿到自己那份 lease 引用。
	// sink 如果要长时间处理，应该尽快 RGA/copy 到自己的 buffer pool，
	// 然后释放 packet/lease，让摄像头 buffer 可以回到采集线程 QBUF。
	// onFrame 可以做一次快速硬件处理，但不能做编码等待、写盘、网络等慢操作。
	// onFrame 不应该抛异常；错误由 sink 自己记录、丢帧或释放私有资源。
	virtual void onFrame(FramePacket packet) = 0;
};
