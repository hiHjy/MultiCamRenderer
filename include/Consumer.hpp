#pragma once
#include "VideoFrame.hpp"

class Consumer {
public:
	virtual ~Consumer() = default;
	// frame 只在 onFrame 调用期间有效。
    // 如果 Consumer 需要异步处理，必须在 onFrame 内部立即拷贝到自己的 buffer。
    // onFrame 必须快速返回，不能做长时间阻塞操作。
	virtual void onFrame(const VideoFrame& frame) = 0;
};
