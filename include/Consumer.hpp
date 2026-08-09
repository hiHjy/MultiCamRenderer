#pragma once

#include "VideoFrame.hpp"

class Consumer {
public:
	virtual ~Consumer() = default;

	// 帧回调：packet.lease 保护底层 V4L2 buffer 生命周期。
	// sink 如果要长时间处理，应该尽快 RGA/copy 到自己的 buffer pool，
	// 然后释放 packet/lease，让摄像头 buffer 可以回到采集线程 QBUF。
	// onFrame 可以做一次快速硬件处理，但不能做编码等待、写盘、网络等慢操作。
	virtual void onFrame(const FramePacket& packet) = 0;
};
