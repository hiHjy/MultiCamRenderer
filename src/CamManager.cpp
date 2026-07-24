#include "CamManager.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <poll.h>
#include <utility>
#include <vector>

bool CamManager::addCamera(const CameraConfig& config)
{
    if (config.cameraId < 0) {
        setError("cameraId 不能小于 0");
        return false;
    }

    if (config.devicePath.empty()) {
        setError("devicePath 不能为空");
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

    CameraSlot slot {};
    slot.config = config;
    slot.source = std::make_unique<V4L2CameraSource>(config.cameraId);
    slot.state = CameraState::Created;

    V4L2CameraSource& camera = *slot.source;
    if (!camera.openDevice(config.devicePath)) {
        slot.state = CameraState::Error;
        slot.lastError = "打开摄像头失败: " + camera.lastError();
        setError(slot.lastError);
        return false;
    }

    if (!camera.configure(config.videoConfig)) {
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
    m_cameraMap.emplace(config.cameraId, std::move(slot));
    m_lastError.clear();
    return true;
}

bool CamManager::delCamera(int cameraId)
{
    std::lock_guard<std::mutex> lock(m_camChangeMutex);
    auto it = m_cameraMap.find(cameraId);
    if (it == m_cameraMap.end()) {
        setError("cameraId 不存在");
        return false;
    }

    if (it->second.source) {
        it->second.source->stop();
        it->second.state = CameraState::Stopped;
    }
    m_cameraMap.erase(it);
    m_lastError.clear();
    return true;
}

bool CamManager::startAll()
{
    std::lock_guard<std::mutex> lock(m_camChangeMutex);
    for (auto& item : m_cameraMap) {
        CameraSlot& slot = item.second;
        if (!slot.source) {
            slot.state = CameraState::Error;
            slot.lastError = "摄像头对象为空";
            setError(slot.lastError);
            return false;
        }

        V4L2CameraSource& camera = *slot.source;
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

void CamManager::stopAll()
{
    std::lock_guard<std::mutex> lock(m_camChangeMutex);
    for (auto& item : m_cameraMap) {
        CameraSlot& slot = item.second;
        if (slot.source) {
            slot.source->stop();
            slot.state = CameraState::Stopped;
        }
    }
    m_stopRequested = true;
}

bool CamManager::pollOnce(int timeoutMs)
{
    std::vector<pollfd> fds;
    std::vector<V4L2CameraSource*> cameras;

    {
        std::lock_guard<std::mutex> lock(m_camChangeMutex);
        fds.reserve(m_cameraMap.size());
        cameras.reserve(m_cameraMap.size());

        // 这里复制 fd 和裸指针快照后立刻释放锁，避免 poll 阻塞期间卡住
        // addCamera/stopAll 等管理操作。
        //
        // 风险点：m_cameraMap 用 unique_ptr 持有摄像头，裸指针快照不延长对象生命周期。
        // 当前第一版约定 pollOnce/run 期间不并发 delCamera。
        // 如果后续要支持运行中删除摄像头，需要改成事件队列，或在快照里持有 shared_ptr。
        for (auto& item : m_cameraMap) {
            CameraSlot& slot = item.second;
            V4L2CameraSource* camera = slot.source.get();
            if (!camera || slot.state != CameraState::Streaming ||
                !camera->isStreaming() || camera->fd() < 0) {
                continue;
            }

            pollfd pfd {};
            pfd.fd = camera->fd();
            pfd.events = POLLIN | POLLERR | POLLHUP;
            fds.push_back(pfd);
            cameras.push_back(camera);
        }
    }

    if (fds.empty()) {
        setError("没有处于 Streaming 状态的摄像头");
        return false;
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

        V4L2CameraSource* camera = cameras[i];
        if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            setError("摄像头 fd 异常");
            return false;
        }

        if ((revents & POLLIN) == 0) {
            continue;
        }

        V4L2CameraSource::Frame frame {};
        if (!camera->dequeueFrame(frame)) {
            if (!camera->lastError().empty()) {
                setError("dequeueFrame 失败: " + camera->lastError());
                return false;
            }
            continue;
        }

        std::cout << "camera " << camera->cameraId()
                  << " frame seq=" << frame.sequence
                  << " ts_us=" << frame.timestampUs
                  << " bytes=" << frame.bytesUsed
                  << " fd=" << frame.dmaFd
                  << " index=" << frame.index
                  << "\n";

        if (!camera->requeueFrame(frame)) {
            setError("requeueFrame 失败: " + camera->lastError());
            return false;
        }
    }

    m_lastError.clear();
    return true;
}

void CamManager::run(int timeoutMs)
{
    while (!m_stopRequested) {
        if (!pollOnce(timeoutMs)) {
            std::cerr << "CamManager pollOnce 失败: " << lastError() << "\n";
            break;
        }
    }
}

void CamManager::requestStop()
{
    m_stopRequested = true;
}

const std::string& CamManager::lastError() const
{
    return m_lastError;
}

void CamManager::setError(const std::string& message)
{
    m_lastError = message;
}
