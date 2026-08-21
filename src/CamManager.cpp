#include "CamManager.hpp"
#include "FrameHub.hpp"
#include "Log.hpp"
#include "VideoFrame.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

PixelFormat toV4L2PixelFormat(PixelFormat format) {
	switch (format) {
	case PixelFormat::Unknown:
		return PixelFormat::Auto;
	case PixelFormat::Auto:
		return PixelFormat::Auto;
	case PixelFormat::NV12:
		return PixelFormat::NV12;
	case PixelFormat::YUYV:
		return PixelFormat::YUYV;
	case PixelFormat::YUV420P:
		return PixelFormat::YUV420P;
	case PixelFormat::RGBA8888:
		return PixelFormat::Auto;
	}
	return PixelFormat::Auto;
}

V4L2CameraSource::CamConfig toV4L2CameraConfig(const CamManager::CameraConfig &config) {
	V4L2CameraSource::CamConfig v4l2Config{};
	v4l2Config.width = config.width;
	v4l2Config.height = config.height;
	v4l2Config.fps = config.fps;
	v4l2Config.format = toV4L2PixelFormat(config.format);
	return v4l2Config;
}

} // namespace

CamManager::CamManager() {
	m_returnEventFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (m_returnEventFd < 0) {
		setError(std::string("eventfd 创建失败: ") + std::strerror(errno));
	}
}

CamManager::~CamManager() {
	if (m_returnEventFd >= 0) {
		close(m_returnEventFd);
		m_returnEventFd = -1;
	}
}

bool CamManager::addCamera(const CameraConfig &config) {
	if (config.cameraId < 0) {
		setError("cameraId 不能小于 0");
		return false;
	}

	if (config.devicePath.empty()) {
		setError("devicePath 不能为空");
		return false;
	}

	if (config.width <= 0 || config.height <= 0) {
		setError("摄像头宽高必须大于 0");
		return false;
	}

	if (config.bufferCount <= 0) {
		setError("bufferCount 必须大于 0");
		return false;
	}

	std::lock_guard<std::mutex> lock(m_camChangeMutex);
	if (m_cameraMap.find(config.cameraId) != m_cameraMap.end()) {
		setError("cameraId 已存在");
		return false;
	}

	CameraSlot slot{};
	slot.config = config;
	slot.source = std::make_shared<V4L2CameraSource>(config.cameraId);
	slot.state = CameraState::Created;

	V4L2CameraSource &camera = *slot.source;
	if (!camera.openDevice(config.devicePath)) {
		slot.state = CameraState::Error;
		slot.lastError = "打开摄像头失败: " + camera.lastError();
		setError(slot.lastError);
		return false;
	}

	const V4L2CameraSource::CamConfig v4l2Config = toV4L2CameraConfig(config);
	if (!camera.configure(v4l2Config)) {
		slot.state = CameraState::Error;
		slot.lastError = "配置摄像头失败: " + camera.lastError();
		setError(slot.lastError);
		return false;
	}

	if (!camera.setupDmaImportBuffers(config.bufferCount, config.dmaHeapPath)) {
		slot.state = CameraState::Error;
		slot.lastError = "准备摄像头 DMA buffer 失败: " + camera.lastError();
		setError(slot.lastError);
		return false;
	}

	slot.state = CameraState::Ready;
	auto hub = std::make_shared<FrameHub>(config.cameraId);

	m_cameraMap.emplace(config.cameraId, std::move(slot));
	m_frameHubMap.emplace(config.cameraId, std::move(hub));
	m_lastError.clear();

	return true;
}

bool CamManager::addFrameSink(int cameraId, std::shared_ptr<Sink> sink) {
	if (!sink) {
		setError("sink 不能为空");
		return false;
	}

	std::lock_guard<std::mutex> lock(m_camChangeMutex);
	auto it = m_frameHubMap.find(cameraId);
	if (it == m_frameHubMap.end() || !it->second) {
		setError("cameraId 对应的 FrameHub 不存在");
		return false;
	}

	if (!it->second->addSink(std::move(sink))) {
		setError("添加 sink 失败: " + it->second->lastError());
		return false;
	}

	m_lastError.clear();
	return true;
}

bool CamManager::addSinkForHub(int cameraId, std::shared_ptr<Sink> sink) {
	return addFrameSink(cameraId, std::move(sink));
}

bool CamManager::delCamera(int cameraId) {
	std::lock_guard<std::mutex> lock(m_camChangeMutex);
	auto it = m_cameraMap.find(cameraId);
	if (it == m_cameraMap.end()) {
		setError("cameraId 不存在");
		return false;
	}

	// delCamera 只摘除 CamManager 对 camera/hub 的管理引用。
	// 如果 pollOnce() 或某个 FrameLease 仍持有 source shared_ptr，
	// V4L2CameraSource 会等最后一个引用释放后再析构清理。
	m_cameraMap.erase(it);
	m_frameHubMap.erase(cameraId);
	m_lastError.clear();
	notifyReturnEvent();
	return true;
}

bool CamManager::startAll() {
	std::lock_guard<std::mutex> lock(m_camChangeMutex);
	for (auto &item : m_cameraMap) {
		CameraSlot &slot = item.second;
		if (!slot.source) {
			slot.state = CameraState::Error;
			slot.lastError = "摄像头对象为空";
			setError(slot.lastError);
			return false;
		}

		V4L2CameraSource &camera = *slot.source;
		if (slot.state == CameraState::Streaming || camera.isStreaming()) {
			slot.state = CameraState::Streaming;
			continue;
		}

		if (!camera.start()) {
			slot.state = CameraState::Error;
			slot.lastError = "启动摄像头失败: " + camera.lastError();
			setError(slot.lastError);
			return false;
		}

		slot.state = CameraState::Streaming;
		slot.lastError.clear();
	}

	m_stopRequested = false;
	m_lastError.clear();
	return true;
}

void CamManager::stopAll() {
	{
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		for (auto &item : m_cameraMap) {
			CameraSlot &slot = item.second;
			if (slot.source) {
				slot.source->stop();
				slot.state = CameraState::Stopped;
			}
		}
		m_stopRequested = true;
	}
	notifyReturnEvent();
}

bool CamManager::pollOnce(int timeoutMs) {
	if (!drainReturnedFrames()) {
		return false;
	}

	std::vector<pollfd> fds;
	std::vector<std::shared_ptr<V4L2CameraSource>> cameras;
	std::vector<std::shared_ptr<FrameHub>> hubs;
	bool hasCameraFd = false;

	{
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		fds.reserve(m_cameraMap.size() + 1);
		cameras.reserve(m_cameraMap.size() + 1);
		hubs.reserve(m_cameraMap.size() + 1);

		// 这里复制 fd 和 shared_ptr 快照后立刻释放锁，避免 poll 阻塞期间卡住
		// addCamera/stopAll 等管理操作，也避免 delCamera() 后快照对象悬空。
		//
		// 注意：shared_ptr 快照只解决对象生命周期，不解决完整热插拔状态机。
		// 后续若要严格支持运行中删除摄像头，仍建议把增删操作改成事件队列。
		for (auto &item : m_cameraMap) {
			CameraSlot &slot = item.second;
			std::shared_ptr<V4L2CameraSource> camera = slot.source;
			if (!camera || slot.state != CameraState::Streaming || !camera->isStreaming() || camera->fd() < 0) {
				continue;
			}

			pollfd pfd{};
			pfd.fd = camera->fd();
			pfd.events = POLLIN | POLLERR | POLLHUP;
			fds.push_back(pfd);
			cameras.push_back(camera);
			auto hubIt = m_frameHubMap.find(camera->cameraId());
			hubs.push_back(hubIt == m_frameHubMap.end() ? nullptr : hubIt->second);
			hasCameraFd = true;
		}
	}

	if (!hasCameraFd) {
		setError("没有处于 Streaming 状态的摄像头");
		return false;
	}

	if (m_returnEventFd >= 0) {
		pollfd pfd{};
		pfd.fd = m_returnEventFd;
		pfd.events = POLLIN | POLLERR | POLLHUP;
		fds.push_back(pfd);
		cameras.push_back(nullptr);
		hubs.push_back(nullptr);
	}

	const int ret = ::poll(fds.data(), fds.size(), timeoutMs);
	if (ret < 0) {
		if (errno == EINTR) {
			return true;
		}
		setError(std::string("poll 失败: ") + std::strerror(errno));
		return false;
	}

	if (ret == 0) {
		return true;
	}

	for (size_t i = 0; i < fds.size(); ++i) {
		const short revents = fds[i].revents;
		if (revents == 0) {
			continue;
		}

		std::shared_ptr<V4L2CameraSource> camera = cameras[i];
		std::shared_ptr<FrameHub> hub = hubs[i];

		if (camera == nullptr) {
			if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
				setError("return eventfd 异常");
				return false;
			}

			if ((revents & POLLIN) != 0 && (!drainReturnEvent() || !drainReturnedFrames())) {
				return false;
			}
			continue;
		}

		if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			setError("摄像头 fd 异常");
			return false;
		}

		if ((revents & POLLIN) == 0) {
			continue;
		}

		V4L2CameraSource::Frame frame{};
		if (!camera->dequeueFrame(frame)) {
			if (!camera->lastError().empty()) {
				setError("dequeueFrame 失败: " + camera->lastError());
				return false;
			}
			continue;
		}

		// LOG_DEBUG("CamManager", "camera=" << camera->cameraId()
		//           << " frame seq=" << frame.sequence
		//           << " ts_us=" << frame.timestampUs
		//           << " bytes=" << frame.bytesUsed
		//           << " fd=" << frame.dmaFd
		//           << " index=" << frame.index);

		VideoFrame videoFrame{
			.streamId = camera->cameraId(),
			.dmaFd = frame.dmaFd,
			.va = frame.va,
			.capacity = frame.capacity,
			.bytesUsed = frame.bytesUsed,
			.width = frame.width,
			.height = frame.height,
			.stride = frame.stride,
			.heightStride = frame.heightStride,
			.format = frame.format,
			.nativeFormat = frame.v4l2Format,
			.timestampUs = frame.timestampUs,
			.sequence = frame.sequence,
			.bufferIndex = frame.index,
		};

		bool publishOk = true;
		std::string publishError;
		FramePacket packet{
			.frame = videoFrame,
			.lease = std::make_shared<FrameLease>(
				[this, sourceLifetime = camera, cameraId = camera->cameraId(), bufferIndex = frame.index]() {
					(void)sourceLifetime;
					postReturnedFrame(cameraId, bufferIndex);
				}),
		};

		bool stillActive = false;
		{
			std::lock_guard<std::mutex> lock(m_camChangeMutex);
			auto cameraIt = m_cameraMap.find(camera->cameraId());
			auto hubIt = m_frameHubMap.find(camera->cameraId());

			stillActive = cameraIt != m_cameraMap.end() && hubIt != m_frameHubMap.end() &&
						  cameraIt->second.source == camera && hubIt->second == hub &&
						  cameraIt->second.state == CameraState::Streaming;
		}

		if (!stillActive) {
			LOG_INFO("CamManager", "cameraId=" << camera->cameraId() << " 已删除或已停止，跳过本帧发布");
			packet.lease.reset();
			if (!drainReturnedFrames()) {
				return false;
			}
			continue;
		}

		if (!hub) {
			publishOk = false;
			publishError = "cameraId 对应的 FrameHub 不存在";
		} else {
			publishOk = hub->publishFrame(packet);
			if (!publishOk) {
				publishError = hub->lastError();
			}
		}

		if (!publishOk) {
			setError("publishFrame 失败: " + publishError);
			return false;
		}

		packet.lease.reset();
		if (!drainReturnedFrames()) {
			return false;
		}
	}

	m_lastError.clear();
	return true;
}

void CamManager::run(int timeoutMs) {
	while (!m_stopRequested) {
		if (!pollOnce(timeoutMs)) {
			LOG_ERROR("CamManager", "pollOnce failed: " << lastError());
			break;
		}
	}
}

void CamManager::requestStop() {
	m_stopRequested = true;
	notifyReturnEvent();
}

const std::string &CamManager::lastError() const {
	return m_lastError;
}

void CamManager::setError(const std::string &message) {
	m_lastError = message;
}

void CamManager::postReturnedFrame(int cameraId, int bufferIndex) {
	{
		std::lock_guard<std::mutex> lock(m_returnMutex);
		m_returnQueue.emplace(cameraId, bufferIndex);
	}

	notifyReturnEvent();
}

void CamManager::notifyReturnEvent() {
	if (m_returnEventFd < 0)
		return;

	uint64_t value = 1;
	ssize_t ret = 0;
	do {
		ret = write(m_returnEventFd, &value, sizeof(value));
	} while (ret < 0 && errno == EINTR);

	// eventfd 计数器满时说明 poll 线程已经有待处理通知。
	// 这里可能由 sink 线程调用，保持非致命，避免跨线程写 m_lastError。
	(void)ret;
}

bool CamManager::drainReturnEvent() {
	if (m_returnEventFd < 0)
		return true;

	uint64_t value = 0;
	while (true) {
		// eventfd 默认不是 EFD_SEMAPHORE 模式：
		// 一次 read 会读出当前累计计数，并把计数清零。
		// value 只表示“期间收到过多少次唤醒”，这里不需要逐个使用，
		// 真正要归还哪些 buffer 以后面的 drainReturnedFrames() 队列为准。
		const ssize_t ret = read(m_returnEventFd, &value, sizeof(value));
		if (ret == static_cast<ssize_t>(sizeof(value))) {
			// 成功读掉一批通知。继续读一次，是为了把同时到来的通知也清空；
			// 读到 EAGAIN 时才说明 eventfd 已经彻底没通知了。
			continue;
		}

		if (ret < 0 && errno == EINTR) {
			// read 被信号打断，不代表 eventfd 出错，重试即可。
			continue;
		}

		if (ret < 0 && errno == EAGAIN) {
			// 非阻塞 fd 在没有数据可读时返回 EAGAIN。
			// 对这里来说，这正是“通知已经读空”的成功条件。
			return true;
		}

		// 其他错误才是真失败，比如 fd 异常关闭或内核返回了非预期错误。
		setError(std::string("读 return eventfd 失败: ") + std::strerror(errno));
		return false;
	}
}

bool CamManager::drainReturnedFrames() {
	std::queue<std::pair<int, int>> pending;
	{
		std::lock_guard<std::mutex> lock(m_returnMutex);
		pending.swap(m_returnQueue);
	}

	if (pending.empty()) {
		return true;
	}

	std::lock_guard<std::mutex> lock(m_camChangeMutex);
	while (!pending.empty()) {
		const std::pair<int, int> item = pending.front();
		pending.pop();
		const int cameraId = item.first;
		const int bufferIndex = item.second;

		auto it = m_cameraMap.find(cameraId);
		if (it == m_cameraMap.end() || !it->second.source) {
			// 摄像头可能已经被 delCamera() 移除。
			// 如果对应 FrameLease 捕获的 sourceLifetime 是最后一个引用，
			// 这里跳过 QBUF，让 V4L2CameraSource 析构时关闭 fd/释放 DMA buffer。
			// 注意：当前 return queue 只记录 cameraId/bufferIndex。
			// 在旧 lease 完全释放前不要复用同一个 cameraId，否则旧归还事件
			// 可能和新 cameraId 混淆。后续应改成内部生成 cameraId 或增加 generation。
			continue;
		}

		V4L2CameraSource::Frame frame{};
		frame.index = bufferIndex;
		if (!it->second.source->requeueFrame(frame)) {
			setError("requeueFrame 失败: " + it->second.source->lastError());
			return false;
		}
	}

	return true;
}
