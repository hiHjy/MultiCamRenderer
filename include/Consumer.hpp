#pragma once

#include "VideoFrame.hpp"

class Consumer {
public:
	virtual ~Consumer() = default;

	// 同步帧回调：frame 只在 onFrame 调用期间有效。
	// 如果 Consumer 需要异步处理，必须在 onFrame 内部立即拷贝到自己的 buffer。
	// onFrame 必须快速返回，不能做长时间阻塞操作，否则会拖住摄像头 requeue。
	// 第一版由 Consumer 自己负责慢处理隔离，例如 RGA copy 到私有 buffer 后返回。
	virtual void onFrame(const VideoFrame& frame) = 0;
};
