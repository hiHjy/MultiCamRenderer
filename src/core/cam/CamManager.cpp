#include "CamManager.hpp"
#include "FrameHub.hpp"
#include "Log.hpp"
#include "VideoFrame.hpp"
#include "VideoTypes.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
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

const char* pixelFormatName(PixelFormat format) {
	switch (format) {
	case PixelFormat::Unknown:
		return "Unknown";
	case PixelFormat::Auto:
		return "Auto";
	case PixelFormat::NV12:
		return "NV12";
	case PixelFormat::YUYV:
		return "YUYV";
	case PixelFormat::YUV420P:
		return "YUV420P";
	case PixelFormat::RGBA8888:
		return "RGBA8888";
	case PixelFormat::MJPEG:
		return "MJPEG";
	}
	return "Unknown";
}

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

std::chrono::seconds restartBackoff(uint32_t attempt) {
	constexpr std::array<int, 4> kBackoffSeconds {1, 2, 5, 10};
	const size_t index = attempt == 0 ? 0 : std::min<size_t>(attempt - 1, kBackoffSeconds.size() - 1);
	return std::chrono::seconds(kBackoffSeconds[index]);
}

std::string pollEventsText(short revents) {
	std::string result;
	auto append = [&result](const char* name) {
		if (!result.empty()) {
			result += '|';
		}
		result += name;
	};
	if ((revents & POLLERR) != 0) append("POLLERR");
	if ((revents & POLLHUP) != 0) append("POLLHUP");
	if ((revents & POLLNVAL) != 0) append("POLLNVAL");
	return result.empty() ? "unknown" : result;
}

} // namespace

CamManager::DecodeWorker::DecodeWorker(int cameraId,
									   int outputBufferCount,
									   size_t outputBufferSize,
									   int outputWidth,
									   int outputHeight,
									   int outputStride,
									   int outputHeightStride,
									   const std::string& dmaHeapPath,
									   std::weak_ptr<FrameHub> hub)
	: m_cameraId(cameraId),
	  m_outputWidth(outputWidth),
	  m_outputHeight(outputHeight),
	  m_outputStride(outputStride),
	  m_outputHeightStride(outputHeightStride),
	  m_hub(std::move(hub)),
	  m_outputPool(std::make_shared<DmaBufferPool>()) {
	if (outputBufferCount <= 0) {
		setError("DecodeWorker outputBufferCount 必须大于 0");
		return;
	}

	if (m_outputWidth <= 0 || m_outputHeight <= 0 ||
		m_outputStride <= 0 || m_outputHeightStride <= 0) {
		setError("DecodeWorker 输出 layout 无效");
		return;
	}

	if (outputBufferSize == 0) {
		setError("DecodeWorker outputBufferSize 不能为 0");
		return;
	}

	if (!m_decoder.init(MppCodec::MJPEG)) {
		setError("初始化 MJPEG 解码器失败: " + m_decoder.lastError());
		return;
	}

	if (!m_outputPool->init(outputBufferCount, outputBufferSize, dmaHeapPath)) {
		setError("初始化 MJPEG 解码输出池失败: " + m_outputPool->lastError());
		return;
	}

	m_initialized = true;
	m_lastError.clear();
	m_workerThread = std::thread(&DecodeWorker::workerLoop, this);
	LOG_INFO("CamManager", "cameraId=" << m_cameraId
						   << " MJPEG DecodeWorker 初始化完成"
						   << " outputSize=" << m_outputWidth << "x" << m_outputHeight
						   << " outputStride=" << m_outputStride
						   << " outputHeightStride=" << m_outputHeightStride
						   << " outputBuffers=" << outputBufferCount
						   << " outputBufferSize=" << outputBufferSize);
}

CamManager::DecodeWorker::~DecodeWorker() {
	shutdown();
}

bool CamManager::DecodeWorker::initialized() const {
	return m_initialized;
}

bool CamManager::DecodeWorker::postFrame(FramePacket packet) {
	if (!m_initialized) {
		setError("DecodeWorker 尚未初始化");
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_stopping) {
			setError("DecodeWorker 正在停止");
			return false;
		}

		if (m_pendingPacket.has_value()) {
			++m_droppedFrames;
			LOG_WARN("CamManager", "cameraId=" << m_cameraId
								   << " MJPEG decode pending frame dropped count="
								   << m_droppedFrames);
		}
		m_pendingPacket = std::move(packet);
	}

	m_cv.notify_one();
	return true;
}

void CamManager::DecodeWorker::shutdown() {
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stopping = true;
		m_pendingPacket.reset();
	}

	m_cv.notify_one();
	if (m_workerThread.joinable()) {
		m_workerThread.join();
	}
}

const std::string& CamManager::DecodeWorker::lastError() const {
	return m_lastError;
}

void CamManager::DecodeWorker::workerLoop() {
	while (true) {
		FramePacket packet;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait(lock, [this] {
				return m_stopping || m_pendingPacket.has_value();
			});

			if (m_stopping && !m_pendingPacket) {
				break;
			}

			packet = std::move(*m_pendingPacket);
			m_pendingPacket.reset();
		}

		processFrame(std::move(packet));
	}
}

void CamManager::DecodeWorker::processFrame(FramePacket packet) {
	if (!m_outputPool) {
		setError("DecodeWorker 输出池不存在");
		return;
	}

	VideoFrame* outputFrame = m_outputPool->acquireFrame();
	if (outputFrame == nullptr) {
		LOG_WARN("CamManager", "cameraId=" << m_cameraId
							   << " MJPEG decode output pool busy，丢弃本帧: "
							   << m_outputPool->lastError());
		return;
	}

	outputFrame->width = m_outputWidth;
	outputFrame->height = m_outputHeight;
	outputFrame->stride = m_outputStride;
	outputFrame->heightStride = m_outputHeightStride;
	outputFrame->format = PixelFormat::NV12;

	if (!m_decoder.decodeMjpeg(packet.frame, *outputFrame)) {
		const std::string error = "MJPEG 解码失败: " + m_decoder.lastError();
		(void)m_outputPool->releaseFrame(outputFrame);
		setError(error);
		LOG_ERROR("CamManager", "cameraId=" << m_cameraId << ' ' << error);
		return;
	}

	packet.lease.reset();

	outputFrame->streamId = m_cameraId;
	FramePacket decodedPacket {
		.frame = *outputFrame,
		.lease = std::make_shared<FrameLease>(
			[pool = m_outputPool, outputFrame]() {
				(void)pool->releaseFrame(outputFrame);
			}),
	};

	std::shared_ptr<FrameHub> hub = m_hub.lock();
	if (!hub) {
		return;
	}

	if (!hub->publishFrame(decodedPacket)) {
		setError("发布 MJPEG 解码帧失败: " + hub->lastError());
		LOG_ERROR("CamManager", "cameraId=" << m_cameraId << ' ' << m_lastError);
	}
}

void CamManager::DecodeWorker::setError(const std::string& message) {
	m_lastError = message;
}

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

	LOG_INFO("CamManager", "cameraId=" << cameraId
						   << " request config"
						   << " device=" << config.devicePath
						   << " size=" << config.width << "x" << config.height
						   << " fps=" << config.fps
						   << " format=" << pixelFormatName(config.format)
						   << " buffers=" << config.bufferCount
						   << " dmaHeap=" << config.dmaHeapPath);

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
	V4L2CameraSource::CamConfig actualConfig{};
	if (!camera.getCurrentConfig(actualConfig)) {
		slot.state = CameraState::Error;
		slot.lastError = "获取摄像头实际配置失败: " + camera.lastError();
		setError(slot.lastError);
		return -1;
	}

	slot.config.width = actualConfig.width;
	slot.config.height = actualConfig.height;
	slot.config.fps = actualConfig.fps;
	slot.config.format = actualConfig.format;
	slot.config.bufferCapacity = actualConfig.bufferCapacity;

	LOG_INFO("CamManager", "cameraId=" << cameraId
						   << " accepted config"
						   << " device=" << config.devicePath
						   << " size=" << actualConfig.width << "x" << actualConfig.height
						   << " fps=" << actualConfig.fps
						   << " format=" << pixelFormatName(actualConfig.format)
						   << " inputBufferCapacity=" << actualConfig.bufferCapacity
						   << " buffers=" << config.bufferCount);

	if (actualConfig.format == PixelFormat::MJPEG) {
		const int outputStride = videoFrameAlignedStride(PixelFormat::NV12, actualConfig.width, 16);
		const int outputHeightStride =
			videoFrameAlignedHeightStride(PixelFormat::NV12, actualConfig.height, 16);
		const size_t outputBufferSize =
			videoFrameBufferSizeFor(PixelFormat::NV12,
									outputStride,
									outputHeightStride,
									VideoBufferSizeMode::MppDecoderOutput);
		LOG_INFO("CamManager", "cameraId=" << cameraId
							   << " MJPEG decode output config"
							   << " format=NV12"
							   << " size=" << actualConfig.width << "x" << actualConfig.height
							   << " stride=" << outputStride
							   << " heightStride=" << outputHeightStride
							   << " outputBufferSize=" << outputBufferSize
							   << " outputBuffers=" << config.bufferCount);
		slot.decodeWorker = std::make_unique<DecodeWorker>(cameraId,
															config.bufferCount,
															outputBufferSize,
															actualConfig.width,
															actualConfig.height,
															outputStride,
															outputHeightStride,
															config.dmaHeapPath,
															hub);
		if (!slot.decodeWorker->initialized()) {
			slot.state = CameraState::Error;
			slot.lastError = slot.decodeWorker->lastError();
			setError(slot.lastError);
			return -1;
		}
	}

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

bool CamManager::requestCameraRecovery(int cameraId, RecoveryMode mode, const std::string& reason) {
	Command command {CommandType::ProcessError, cameraId};
	std::string message = reason.empty() ? "外部请求摄像头恢复" : reason;
	{
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		auto it = m_cameraMap.find(cameraId);
		if (it == m_cameraMap.end() || it->second.state == CameraState::Deleting || !it->second.source) {
			setError("cameraId 不存在、正在删除或摄像头对象为空");
			return false;
		}
		if (it->second.state == CameraState::Error) {
			setError("cameraId 正在恢复中");
			return false;
		}

		CameraSlot& slot = it->second;
		slot.state = CameraState::Error;
		slot.lastError = message;
		slot.runtime.lastRevents = 0;
		slot.runtime.requiresFullRecovery = mode == RecoveryMode::ReopenDevice;
		++slot.runtime.errorGeneration;
		command.errorGeneration = slot.runtime.errorGeneration;
	}

	setError(message);
	LOG_WARN("CamManager", "cameraId=" << cameraId << " 收到外部恢复请求 mode="
								 << (mode == RecoveryMode::ReopenDevice ? "ReopenDevice" : "RestartStream")
								 << " reason=" << message);
	return postCommand(command);
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
	// 恢复到期后仍通过命令队列执行，确保 V4L2 状态机只有 poll 线程碰。
	enqueueDueRestartCommands();
	if (!drainCommands()) {
		return false;
	}

	if (!drainReturnedFrames()) {
		return false;
	}
	enqueueDueRestartCommands();
	if (!drainCommands()) {
		return false;
	}

	std::vector<pollfd> fds;
	std::vector<std::shared_ptr<V4L2CameraSource>> cameras;
	std::vector<std::shared_ptr<FrameHub>> hubs;
	std::vector<DecodeWorker*> decodeWorkers;
	bool hasCameraFd = false;

	{
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		fds.reserve(m_cameraMap.size() + 1);
		cameras.reserve(m_cameraMap.size() + 1);
		hubs.reserve(m_cameraMap.size() + 1);
		decodeWorkers.reserve(m_cameraMap.size() + 1);

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
			decodeWorkers.push_back(slot.decodeWorker.get());
			hasCameraFd = true;
		}
	}

	if (!hasCameraFd) {
		std::unique_lock<std::mutex> lock(m_camChangeMutex);
		auto shouldWake = [this] {
			for (const auto &item : m_cameraMap) {
				const auto &camSlot = item.second;
				const auto &camera = camSlot.source;
				if (camera && camSlot.state == CameraState::Streaming &&
					camera->isStreaming() && camera->fd() >= 0) {
					return true;
				}
			}

			return m_stopRequested.load() || m_hasPendingCommand.load() ||
				m_hasPendingReturnedFrame.load();
		};

		std::chrono::steady_clock::time_point nextRestart {};
		for (const auto& item : m_cameraMap) {
			const CameraRuntimeStatus& runtime = item.second.runtime;
			if (item.second.state != CameraState::Error || !runtime.restartPending ||
				runtime.restartQueued || runtime.inFlightFrames != 0) {
				continue;
			}
			if (nextRestart == std::chrono::steady_clock::time_point {} ||
				runtime.restartNotBefore < nextRestart) {
				nextRestart = runtime.restartNotBefore;
			}
		}

		if (nextRestart == std::chrono::steady_clock::time_point {}) {
			m_camCv.wait(lock, shouldWake);
		} else {
			m_camCv.wait_until(lock, nextRestart, shouldWake);
		}

		return true;
	}

	if (m_returnEventFd >= 0) {
		pollfd pfd{};
		pfd.fd = m_returnEventFd;
		pfd.events = POLLIN | POLLERR | POLLHUP;
		fds.push_back(pfd);
		cameras.push_back(nullptr);
		hubs.push_back(nullptr);
		decodeWorkers.push_back(nullptr);
	}

	const int ret = ::poll(fds.data(), fds.size(), recoveryAwarePollTimeoutMs(timeoutMs));
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
		DecodeWorker* decodeWorker = decodeWorkers[i];

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
			reportCameraFault(camera->cameraId(),
							  camera,
							  revents,
							  "摄像头 fd 异常: " + pollEventsText(revents));
			continue;
		}

		if ((revents & POLLIN) == 0) {
			continue;
		}

		V4L2CameraSource::Frame frame{};
		if (!camera->dequeueFrame(frame)) {
			if (!camera->lastError().empty()) {
				reportCameraFault(camera->cameraId(),
							  camera,
							  0,
							  "dequeueFrame 失败: " + camera->lastError());
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
		bool stillActive = false;
		uint64_t leaseGeneration = 0;
		{
			std::lock_guard<std::mutex> lock(m_camChangeMutex);
			auto cameraIt = m_cameraMap.find(camera->cameraId());
			auto hubIt = m_frameHubMap.find(camera->cameraId());

			stillActive = cameraIt != m_cameraMap.end() && hubIt != m_frameHubMap.end() &&
						  cameraIt->second.source == camera && hubIt->second == hub &&
						  cameraIt->second.state == CameraState::Streaming;
			if (stillActive) {
				CameraRuntimeStatus& runtime = cameraIt->second.runtime;
				++runtime.inFlightFrames;
				leaseGeneration = runtime.leaseGeneration;
			}
		}

		FramePacket packet{
			.frame = videoFrame,
			.lease = std::make_shared<FrameLease>(
				[this,
				 sourceLifetime = camera,
				 cameraId = camera->cameraId(),
				 bufferIndex = frame.index,
				 leaseGeneration]() {
					(void)sourceLifetime;
					postReturnedFrame(cameraId, bufferIndex, leaseGeneration);
				}),
		};

		if (!stillActive) {
			LOG_INFO("CamManager", "cameraId=" << camera->cameraId() << " 已删除或已停止，跳过本帧发布");
			packet.lease.reset();
			if (!drainReturnedFrames()) {
				return false;
			}
			continue;
		}

		if (packet.frame.format == PixelFormat::MJPEG) {
			if (!decodeWorker) {
				publishOk = false;
				publishError = "MJPEG 摄像头缺少 DecodeWorker";
			} else if (!decodeWorker->postFrame(std::move(packet))) {
				publishOk = false;
				publishError = decodeWorker->lastError();
			} else {
				continue;
			}
		} else if (!hub) {
			publishOk = false;
			publishError = "cameraId 对应的 FrameHub 不存在";
		} else {
			publishOk = hub->publishFrame(packet);
			if (!publishOk) {
				publishError = hub->lastError();
			}
		}

		if (!publishOk) {
			reportCameraFault(camera->cameraId(),
							  camera,
							  0,
							  "publishFrame 失败: " + publishError);
			packet.lease.reset();
			if (!drainReturnedFrames()) {
				return false;
			}
			continue;
		}

		packet.lease.reset();
		if (!drainReturnedFrames()) {
			return false;
		}
	}

	return true;
}

void CamManager::run(int timeoutMs) {
	int consecutiveFailures = 0;
	while (!m_stopRequested.load()) {
		if (pollOnce(timeoutMs)) {
			consecutiveFailures = 0;
			continue;
		}

		++consecutiveFailures;
		LOG_ERROR("CamManager", "pollOnce 失败，连续次数=" << consecutiveFailures
								 << " error=" << lastError());
		if (consecutiveFailures >= 10) {
			LOG_ERROR("CamManager", "pollOnce 连续失败达到上限，退出 poll 线程");
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(200));
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

void CamManager::reportCameraFault(int cameraId,
								 const std::shared_ptr<V4L2CameraSource>& expectedSource,
								 short revents,
								 const std::string& message) {
	Command command {CommandType::ProcessError, cameraId};
	bool shouldPost = false;
	{
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		auto it = m_cameraMap.find(cameraId);
		if (it == m_cameraMap.end() || it->second.source != expectedSource ||
			it->second.state == CameraState::Deleting || it->second.state == CameraState::Error) {
			return;
		}

		CameraSlot& slot = it->second;
		slot.state = CameraState::Error;
		slot.lastError = message;
		slot.runtime.lastRevents = revents;
		slot.runtime.requiresFullRecovery =
			(revents & static_cast<short>(POLLHUP | POLLNVAL)) != 0;
		++slot.runtime.errorGeneration;
		command.errorGeneration = slot.runtime.errorGeneration;
		shouldPost = true;
	}

	setError(message);
	LOG_ERROR("CamManager", "cameraId=" << cameraId << ' ' << message
								 << "，已隔离该路并等待串行恢复");
	if (shouldPost) {
		(void)postCommand(command);
	}
}

void CamManager::enqueueDueRestartCommands() {
	std::vector<Command> dueCommands;
	const auto now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		for (auto& item : m_cameraMap) {
			CameraSlot& slot = item.second;
			CameraRuntimeStatus& runtime = slot.runtime;
			if (slot.state == CameraState::Stopped && runtime.startPending) {
				if (runtime.inFlightFrames == 0 && !runtime.startQueued) {
					runtime.startQueued = true;
					dueCommands.push_back(Command {CommandType::StartCamera, item.first});
				}
				continue;
			}

			if (slot.state != CameraState::Error || !runtime.restartPending || runtime.restartQueued) {
				continue;
			}

			if (runtime.inFlightFrames != 0) {
				// 不能为恢复强制释放仍被 Sink 持有的 DMA buffer；这里只做可观测告警。
				constexpr auto kLeaseWaitWarningAfter = std::chrono::seconds(2);
				constexpr auto kLeaseWaitWarningInterval = std::chrono::seconds(10);
				if (runtime.leaseWaitStarted != std::chrono::steady_clock::time_point {} &&
					now - runtime.leaseWaitStarted >= kLeaseWaitWarningAfter &&
					now >= runtime.nextLeaseWaitWarning) {
					LOG_WARN("CamManager", "cameraId=" << item.first << " 等待 "
											   << runtime.inFlightFrames
											   << " 个 FrameLease 归还已超过 2 秒；为避免复用仍在读取的 DMA buffer，暂不强制恢复");
					runtime.nextLeaseWaitWarning = now + kLeaseWaitWarningInterval;
				}
				continue;
			}

			if (now < runtime.restartNotBefore) {
				continue;
			}

			runtime.restartQueued = true;
			dueCommands.push_back(Command {CommandType::RestartCamera, item.first, runtime.errorGeneration});
		}
	}

	for (const Command& command : dueCommands) {
		(void)postCommand(command);
	}
}

int CamManager::recoveryAwarePollTimeoutMs(int timeoutMs) {
	int result = timeoutMs;
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(m_camChangeMutex);
	for (const auto& item : m_cameraMap) {
		const CameraSlot& slot = item.second;
		const CameraRuntimeStatus& runtime = slot.runtime;
		if (slot.state != CameraState::Error || !runtime.restartPending || runtime.restartQueued ||
			runtime.inFlightFrames != 0) {
			continue;
		}

		const auto delay = runtime.restartNotBefore - now;
		const int delayMs = delay <= std::chrono::steady_clock::duration::zero()
			? 0
			: static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(delay).count());
		result = std::min(result, delayMs);
	}
	return std::max(result, 0);
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
		return true;
	}

	V4L2CameraSource& camera = *slot.source;
	switch (command.type) {
	case CommandType::StartCamera:
		if (slot.state == CameraState::Stopped && slot.runtime.inFlightFrames != 0) {
			slot.runtime.startPending = true;
			slot.runtime.startQueued = false;
			LOG_INFO("CamManager", "cameraId=" << command.cameraId << " 等待 "
									 << slot.runtime.inFlightFrames
									 << " 个旧 FrameLease 归还后再启动");
			return true;
		}

		if (slot.state == CameraState::Streaming || camera.isStreaming()) {
			slot.state = CameraState::Streaming;
			slot.lastError.clear();
			slot.runtime.restartAttempts = 0;
			slot.runtime.startPending = false;
			slot.runtime.startQueued = false;
			slot.runtime.restartPending = false;
			slot.runtime.restartQueued = false;
			slot.runtime.requiresFullRecovery = false;
			slot.runtime.leaseWaitStarted = {};
			slot.runtime.nextLeaseWaitWarning = {};
			++slot.runtime.errorGeneration;
			clearError();
			m_camCv.notify_one();
			return true;
		}

		if (!camera.start()) {
			slot.state = CameraState::Error;
			slot.lastError = "启动摄像头失败: " + camera.lastError();
			CameraRuntimeStatus& runtime = slot.runtime;
			runtime.startPending = false;
			runtime.startQueued = false;
			runtime.restartPending = true;
			runtime.restartQueued = false;
			runtime.requiresFullRecovery = false;
			++runtime.errorGeneration;
			++runtime.restartAttempts;
			runtime.restartNotBefore = std::chrono::steady_clock::now() + restartBackoff(runtime.restartAttempts);
			runtime.leaseWaitStarted = std::chrono::steady_clock::now();
			runtime.nextLeaseWaitWarning = runtime.leaseWaitStarted + std::chrono::seconds(2);
			setError(slot.lastError);
			LOG_ERROR("CamManager", "cameraId=" << command.cameraId << ' ' << slot.lastError
								 << "，将在 " << restartBackoff(runtime.restartAttempts).count()
								 << " 秒后重试");
			m_camCv.notify_one();
			return true;
		}

		slot.state = CameraState::Streaming;
		slot.lastError.clear();
		slot.runtime.restartAttempts = 0;
		slot.runtime.startPending = false;
		slot.runtime.startQueued = false;
		slot.runtime.restartPending = false;
		slot.runtime.restartQueued = false;
		slot.runtime.requiresFullRecovery = false;
		slot.runtime.leaseWaitStarted = {};
		slot.runtime.nextLeaseWaitWarning = {};
		++slot.runtime.leaseGeneration;
		clearError();
		m_camCv.notify_one();
		return true;

	case CommandType::StopCamera:
		if (camera.isStreaming()) {
			camera.stop();
		}
		slot.state = CameraState::Stopped;
		slot.lastError.clear();
		slot.runtime.startPending = false;
		slot.runtime.startQueued = false;
		slot.runtime.restartPending = false;
		slot.runtime.restartQueued = false;
		slot.runtime.leaseWaitStarted = {};
		slot.runtime.nextLeaseWaitWarning = {};
		++slot.runtime.errorGeneration;
		clearError();
		m_camCv.notify_one();
		notifyReturnEvent();
		return true;

	case CommandType::ProcessError: {
		if (slot.state != CameraState::Error ||
			command.errorGeneration != slot.runtime.errorGeneration) {
			return true;
		}

		if (camera.isStreaming()) {
			camera.stop();
		}

		CameraRuntimeStatus& runtime = slot.runtime;
		runtime.restartPending = true;
		runtime.restartQueued = false;
		++runtime.restartAttempts;
		const auto now = std::chrono::steady_clock::now();
		runtime.restartNotBefore = now + restartBackoff(runtime.restartAttempts);
		runtime.leaseWaitStarted = now;
		runtime.nextLeaseWaitWarning = now + std::chrono::seconds(2);
		LOG_WARN("CamManager", "cameraId=" << command.cameraId
								 << " 已停止，等待 " << runtime.inFlightFrames
								 << " 个 FrameLease 归还后恢复"
								 << "，第 " << runtime.restartAttempts << " 次将在 "
								 << restartBackoff(runtime.restartAttempts).count() << " 秒后尝试");
		m_camCv.notify_one();
		return true;
	}

	case CommandType::RestartCamera: {
		CameraRuntimeStatus& runtime = slot.runtime;
		if (slot.state != CameraState::Error || !runtime.restartPending ||
			command.errorGeneration != runtime.errorGeneration) {
			return true;
		}

		runtime.restartQueued = false;
		if (runtime.inFlightFrames != 0) {
			return true;
		}

		bool recovered = true;
		std::string recoveryError;
		if (runtime.requiresFullRecovery) {
			camera.closeDevice();
			recovered = camera.openDevice(slot.config.devicePath);
			if (recovered) {
				recovered = camera.configure(toV4L2CameraConfig(slot.config));
			}
			if (recovered) {
				recovered = camera.setupDmaImportBuffers(slot.config.bufferCount, slot.config.dmaHeapPath);
			}
			if (!recovered) {
				recoveryError = "完整重建摄像头失败: " + camera.lastError();
			}
		}

		if (recovered && !camera.start()) {
			recovered = false;
			recoveryError = "恢复 STREAMON 失败: " + camera.lastError();
		}

		if (!recovered) {
			slot.state = CameraState::Error;
			slot.lastError = recoveryError;
			runtime.restartPending = true;
			++runtime.restartAttempts;
			if (!runtime.requiresFullRecovery && runtime.restartAttempts >= 3) {
				runtime.requiresFullRecovery = true;
				LOG_WARN("CamManager", "cameraId=" << command.cameraId
										 << " 轻量恢复已连续失败 " << runtime.restartAttempts
										 << " 次，下一轮升级为完整重建");
			}
			runtime.restartNotBefore = std::chrono::steady_clock::now() + restartBackoff(runtime.restartAttempts);
			setError(slot.lastError);
			LOG_ERROR("CamManager", "cameraId=" << command.cameraId << ' ' << slot.lastError
								 << "，将在 " << restartBackoff(runtime.restartAttempts).count()
								 << " 秒后重试");
			m_camCv.notify_one();
			return true;
		}

		slot.state = CameraState::Streaming;
		slot.lastError.clear();
		runtime.restartAttempts = 0;
		runtime.startPending = false;
		runtime.startQueued = false;
		runtime.restartPending = false;
		runtime.restartQueued = false;
		runtime.requiresFullRecovery = false;
		runtime.lastRevents = 0;
		runtime.leaseWaitStarted = {};
		runtime.nextLeaseWaitWarning = {};
		++runtime.leaseGeneration;
		clearError();
		LOG_INFO("CamManager", "cameraId=" << command.cameraId << " 摄像头恢复成功");
		m_camCv.notify_one();
		return true;
	}

	case CommandType::DeleteCamera:
		return true;
	}

	setError("未知 CamManager 命令");
	return false;
}

void CamManager::postReturnedFrame(int cameraId, int bufferIndex, uint64_t leaseGeneration) {
	{
		std::lock_guard<std::mutex> lock(m_returnMutex);
		m_returnQueue.push(ReturnedFrame {cameraId, bufferIndex, leaseGeneration});
		m_hasPendingReturnedFrame = true;
	}

	m_camCv.notify_one();
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
	std::queue<ReturnedFrame> pending;
	{
		std::lock_guard<std::mutex> lock(m_returnMutex);
		pending.swap(m_returnQueue);
		m_hasPendingReturnedFrame = !m_returnQueue.empty();
	}

	if (pending.empty()) {
		return true;
	}

	std::vector<Command> recoveryCommands;
	std::vector<std::pair<int, std::string>> faults;
	{
		std::lock_guard<std::mutex> lock(m_camChangeMutex);
		while (!pending.empty()) {
			const ReturnedFrame item = pending.front();
			pending.pop();
			const int cameraId = item.cameraId;
			const int bufferIndex = item.bufferIndex;

			auto it = m_cameraMap.find(cameraId);
			if (it == m_cameraMap.end() || !it->second.source) {
				// 摄像头可能已经被 delCamera() 移除。
				// 如果对应 FrameLease 捕获的 sourceLifetime 是最后一个引用，
				// 这里跳过 QBUF，让 V4L2CameraSource 析构时关闭 fd/释放 DMA buffer。
				continue;
			}

			CameraSlot& slot = it->second;
			CameraRuntimeStatus& runtime = slot.runtime;
			if (item.leaseGeneration == 0) {
				// 本帧在创建 lease 前已经 stop/delete；不能对当前代 QBUF。
				continue;
			}

			if (item.leaseGeneration != runtime.leaseGeneration) {
				// 正常 stop/start 会等待旧 lease 清空，因此跨代事件代表旧控制路径或异常时序。
				// 绝不能把旧 buffer index QBUF 到新一代驱动队列。
				LOG_WARN("CamManager", "cameraId=" << cameraId
									   << " 丢弃跨代 FrameLease 归还 leaseGeneration="
									   << item.leaseGeneration << " currentGeneration="
									   << runtime.leaseGeneration);
				continue;
			}

			if (runtime.inFlightFrames > 0) {
				--runtime.inFlightFrames;
			} else {
				LOG_WARN("CamManager", "cameraId=" << cameraId
									   << " 收到多余的 FrameLease 归还通知，已忽略");
			}

			if (slot.state != CameraState::Streaming || !slot.source->isStreaming()) {
				// stopCamera() 后 STREAMOFF 会重置驱动队列。
				// 旧 FrameLease 后续释放时不再 QBUF，下一次 start() 会重新 QBUF 全部 buffer。
				continue;
			}

			V4L2CameraSource::Frame frame{};
			frame.index = bufferIndex;
			if (!slot.source->requeueFrame(frame)) {
				const std::string message = "requeueFrame 失败: " + slot.source->lastError();
				slot.state = CameraState::Error;
				slot.lastError = message;
				slot.runtime.lastRevents = 0;
				slot.runtime.requiresFullRecovery = true;
				++slot.runtime.errorGeneration;
				faults.emplace_back(cameraId, message);
				recoveryCommands.push_back(
					Command {CommandType::ProcessError, cameraId, slot.runtime.errorGeneration});
			}
		}
	}

	for (const auto& fault : faults) {
		setError(fault.second);
		LOG_ERROR("CamManager", "cameraId=" << fault.first << ' ' << fault.second
								 << "，已隔离该路并等待串行恢复");
	}
	for (const Command& command : recoveryCommands) {
		(void)postCommand(command);
	}

	return true;
}
