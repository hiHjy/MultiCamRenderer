#include "CamManager.hpp"
#include "FrameHub.hpp"
#include "Log.hpp"
#include "VideoFrame.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>
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
	case PixelFormat::MJPEG:
		return PixelFormat::MJPEG;
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
	shutdownPolling();
	if (m_returnEventFd >= 0) {
		close(m_returnEventFd);
		m_returnEventFd = -1;
	}
}

int CamManager::addCamera(const CameraConfig &config) {
	if (config.devicePath.empty()) {
		setError("devicePath 不能为空");
		return -1;
	}

	if (config.width <= 0 || config.height <= 0) {
		setError("摄像头宽高必须大于 0");
		return -1;
	}

	if (config.bufferCount <= 0) {
		setError("bufferCount 必须大于 0");
		return -1;
	}

	std::lock_guard<std::mutex> lock(m_camChangeMutex);
	const int cameraId = allocateCameraIdLocked();
	if (cameraId < 0) {
		setError("cameraId 已耗尽");
		return -1;
	}

	CameraSlot slot{};
	slot.config = config;
	slot.source = std::make_shared<V4L2CameraSource>(cameraId);
	slot.state = CameraState::Created;

	V4L2CameraSource &camera = *slot.source;
	if (!camera.openDevice(config.devicePath)) {
		slot.state = CameraState::Error;
		slot.lastError = "打开摄像头失败: " + camera.lastError();
		setError(slot.lastError);
		return -1;
	}

	const V4L2CameraSource::CamConfig v4l2Config = toV4L2CameraConfig(config);
	if (!camera.configure(v4l2Config)) {
		slot.state = CameraState::Error;
		slot.lastError = "配置摄像头失败: " + camera.lastError();
		setError(slot.lastError);
		return -1;
	}

	if (!camera.setupDmaImportBuffers(config.bufferCount, config.dmaHeapPath)) {
		slot.state = CameraState::Error;
		slot.lastError = "准备摄像头 DMA buffer 失败: " + camera.lastError();
		setError(slot.lastError);
		return -1;
	}

	slot.state = CameraState::Ready;
	auto hub = std::make_shared<FrameHub>(cameraId);

	m_cameraMap.emplace(cameraId, std::move(slot));
	m_frameHubMap.emplace(cameraId, std::move(hub));
	clearError();

	return cameraId;
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

	auto cameraIt = m_cameraMap.find(cameraId);
	if (cameraIt == m_cameraMap.end() || cameraIt->second.state == CameraState::Deleting) {
		setError("cameraId 不存在或正在删除");
		return false;
	}

	if (!it->second->addSink(std::move(sink))) {
		setError("添加 sink 失败: " + it->second->lastError());
		return false;
	}

	clearError();
	return true;
}

bool CamManager::addSinkForHub(int cameraId, std::shared_ptr<Sink> sink) {
	return addFrameSink(cameraId, std::move(sink));
}

bool CamManager::delCamera(int cameraId) {
	std::shared_ptr<FrameHub> hubToClose;
	{
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		auto it = m_cameraMap.find(cameraId);
		if (it == m_cameraMap.end()) {
			setError("cameraId 不存在");
			return false;
		}

		it->second.state = CameraState::Deleting;
		auto hubIt = m_frameHubMap.find(cameraId);
		if (hubIt != m_frameHubMap.end()) {
			hubToClose = hubIt->second;
		}
	}

	if (hubToClose) {
		hubToClose->close();
	}

	if (!m_running.load()) {
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		m_cameraMap.erase(cameraId);
		m_frameHubMap.erase(cameraId);
		clearError();
		return true;
	}

	clearError();
	return postCommand(Command{CommandType::DeleteCamera, cameraId});
}

bool CamManager::startCamera(int cameraId) {
	{
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		auto it = m_cameraMap.find(cameraId);
		if (it == m_cameraMap.end() || it->second.state == CameraState::Deleting) {
			setError("cameraId 不存在或正在删除");
			return false;
		}
	}

	clearError();
	return postCommand(Command{CommandType::StartCamera, cameraId});
}

bool CamManager::stopCamera(int cameraId) {
	{
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		auto it = m_cameraMap.find(cameraId);
		if (it == m_cameraMap.end() || it->second.state == CameraState::Deleting) {
			setError("cameraId 不存在或正在删除");
			return false;
		}
	}

	clearError();
	return postCommand(Command{CommandType::StopCamera, cameraId});
}

bool CamManager::startAllCameras() {
	std::vector<int> cameraIds;
	{
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		cameraIds.reserve(m_cameraMap.size());
		for (const auto &item : m_cameraMap) {
			if (item.second.state != CameraState::Deleting) {
				cameraIds.push_back(item.first);
			}
		}
	}

	clearError();
	for (int cameraId : cameraIds) {
		if (!postCommand(Command{CommandType::StartCamera, cameraId})) {
			return false;
		}
	}
	return true;
}

void CamManager::stopAllCameras() {
	std::vector<int> cameraIds;
	{
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		cameraIds.reserve(m_cameraMap.size());
		for (const auto &item : m_cameraMap) {
			if (item.second.state != CameraState::Deleting) {
				cameraIds.push_back(item.first);
			}
		}
	}

	for (int cameraId : cameraIds) {
		(void)postCommand(Command{CommandType::StopCamera, cameraId});
	}
}

bool CamManager::pollOnce(int timeoutMs) {
	if (!drainCommands()) {
		return false;
	}

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
		// addCamera/stopAllCameras 等管理操作，也避免 delCamera() 后快照对象悬空。
		// start/stop 这类会改变 V4L2 fd 状态机的操作已经通过内部命令队列
		// 收敛到 poll 线程执行；delCamera 仍只摘除 shared_ptr 管理引用。
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
		std::unique_lock<std::mutex> lock(m_camChangeMutex);

		m_camCv.wait(lock, [this] {
			for (const auto &item : m_cameraMap) {
				const auto &camSlot = item.second;
				const auto &camera = camSlot.source;
				if (camera && camSlot.state == CameraState::Streaming &&
					camera->isStreaming() && camera->fd() >= 0) {
					return true;
				}
			}

			return m_stopRequested.load() || m_hasPendingCommand.load();
		});

		return true;
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

			if ((revents & POLLIN) != 0 &&
				(!drainReturnEvent() || !drainCommands() || !drainReturnedFrames())) {
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

	clearError();
	return true;
}

void CamManager::run(int timeoutMs) {
	while (!m_stopRequested.load()) {
		if (!pollOnce(timeoutMs)) {
			LOG_ERROR("CamManager", "pollOnce failed: " << lastError());
			break;
		}
	}
	m_running = false;
}

void CamManager::requestStop() {
	m_stopRequested = true;
	m_camCv.notify_one();
	notifyReturnEvent();
}

void CamManager::startPolling()
{
	std::lock_guard<std::mutex> lock(m_threadMutex);

	if (m_running.exchange(true)) {
		LOG_WARN("CamManager", "CamManager is already running");
		return;
	}

	if (m_pollThread.joinable()) {
		m_pollThread.join();
	}

	m_stopRequested = false;
	m_pollThread = std::thread([this] {
		run();
	});
}

void CamManager::shutdownPolling()
{
	std::lock_guard<std::mutex> lock(m_threadMutex);
	if (!m_running.load() && !m_pollThread.joinable()) {
		return;
	}

	m_stopRequested = true;
	m_camCv.notify_one();
	notifyReturnEvent();

	if (m_pollThread.joinable()) {
		m_pollThread.join();
	}
	m_running = false;
}

std::string CamManager::lastError() const {
	std::lock_guard<std::mutex> lock(m_errorMutex);
	return m_lastError;
}

void CamManager::setError(const std::string &message) {
	std::lock_guard<std::mutex> lock(m_errorMutex);
	m_lastError = message;
}

void CamManager::clearError() {
	std::lock_guard<std::mutex> lock(m_errorMutex);
	m_lastError.clear();
}

int CamManager::allocateCameraIdLocked() {
	const int maxCameraId = std::numeric_limits<int>::max();
	while (m_nextCameraId < maxCameraId) {
		const int cameraId = m_nextCameraId;
		++m_nextCameraId;
		if (m_cameraMap.find(cameraId) == m_cameraMap.end()) {
			return cameraId;
		}
	}

	return -1;
}

bool CamManager::postCommand(Command command) {
	// 外部线程只负责提交命令，不直接执行 STREAMON/STREAMOFF。
	// poll 线程被 eventfd/CV 唤醒后会 drainCommands()，再统一操作 V4L2 fd。
	{
		std::lock_guard<std::mutex> lock(m_commandMutex);
		m_commandQueue.push(command);
		m_hasPendingCommand = true;
	}

	m_camCv.notify_one();
	notifyReturnEvent();
	return true;
}

bool CamManager::drainCommands() {
	std::queue<Command> pending;
	{
		std::lock_guard<std::mutex> lock(m_commandMutex);
		pending.swap(m_commandQueue);
		m_hasPendingCommand = false;
	}

	while (!pending.empty()) {
		const Command command = pending.front();
		pending.pop();
		if (!executeCommand(command)) {
			return false;
		}
	}

	return true;
}

bool CamManager::executeCommand(const Command& command) {
	// 这里运行在 CamManager 的 poll 线程上下文中，和 DQBUF/QBUF 串行，
	// 避免外部线程直接 stop/start 时打断同一个 V4L2 fd 的状态机。
	std::lock_guard<std::mutex> lock(m_camChangeMutex);

	auto it = m_cameraMap.find(command.cameraId);
	if (it == m_cameraMap.end()) {
		LOG_INFO("CamManager", "cameraId=" << command.cameraId << " 已不存在，跳过摄像头控制命令");
		return true;
	}

	CameraSlot& slot = it->second;
	if (command.type == CommandType::DeleteCamera) {
		m_cameraMap.erase(command.cameraId);
		m_frameHubMap.erase(command.cameraId);
		clearError();
		m_camCv.notify_one();
		notifyReturnEvent();
		return true;
	}

	if (slot.state == CameraState::Deleting) {
		LOG_INFO("CamManager", "cameraId=" << command.cameraId << " 正在删除，跳过摄像头控制命令");
		return true;
	}

	if (!slot.source) {
		slot.state = CameraState::Error;
		slot.lastError = "摄像头对象为空";
		setError(slot.lastError);
		return false;
	}

	V4L2CameraSource& camera = *slot.source;
	switch (command.type) {
	case CommandType::StartCamera:
		if (slot.state == CameraState::Streaming || camera.isStreaming()) {
			slot.state = CameraState::Streaming;
			slot.lastError.clear();
			clearError();
			m_camCv.notify_one();
			return true;
		}

		if (!camera.start()) {
			slot.state = CameraState::Error;
			slot.lastError = "启动摄像头失败: " + camera.lastError();
			setError(slot.lastError);
			return false;
		}

		slot.state = CameraState::Streaming;
		slot.lastError.clear();
		clearError();
		m_camCv.notify_one();
		return true;

	case CommandType::StopCamera:
		if (camera.isStreaming()) {
			camera.stop();
		}
		slot.state = CameraState::Stopped;
		slot.lastError.clear();
		clearError();
		m_camCv.notify_one();
		notifyReturnEvent();
		return true;

	case CommandType::DeleteCamera:
		return true;
	}

	setError("未知 CamManager 命令");
	return false;
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
			// cameraId 由 CamManager 内部单调生成，不对外复用，避免旧 lease
			// 归还事件误命中新摄像头。
			continue;
		}

		if (it->second.state != CameraState::Streaming || !it->second.source->isStreaming()) {
			// stopCamera() 后 STREAMOFF 会重置驱动队列。
			// 旧 FrameLease 后续释放时不再 QBUF，下一次 start() 会重新 QBUF 全部 buffer。
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
