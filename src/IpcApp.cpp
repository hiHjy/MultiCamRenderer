#include "CamManager.hpp"
#include "Live555RtspServer.hh"
#include "Log.hpp"
#include "RtspPublishSink.hpp"

#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

constexpr char kMainDevicePath[] = "/dev/video33"; // rkvpss_scale0
constexpr char kSubDevicePath[] = "/dev/video34";  // rkvpss_scale1
constexpr unsigned short kRtspPort = 8554;
constexpr int kCameraFps = 30;
constexpr int kCameraBufferCount = 6;

volatile std::sig_atomic_t g_stopRequested = 0;

extern "C" void handleSignal(int)
{
    g_stopRequested = 1;
}

CamManager::CameraConfig makeCameraConfig(const char* devicePath, int width, int height)
{
    CamManager::CameraConfig config {};
    config.devicePath = devicePath;
    config.width = width;
    config.height = height;
    config.fps = kCameraFps;
    config.format = PixelFormat::NV12;
    // 编码 worker 最多持有一张、待编码队列最多一张，6 块留出采集余量。
    config.bufferCount = kCameraBufferCount;
    return config;
}

RtspPublishSink::Config makePublishConfig(const std::string& streamName,
                                          VideoCodec codec,
                                          int width,
                                          int height)
{
    RtspPublishSink::Config config {};
    config.streamName = streamName;
    config.encoderConfig.codec = toMppCodec(codec);
    config.encoderConfig.width = width;
    config.encoderConfig.height = height;
    config.encoderConfig.stride = width;
    config.encoderConfig.heightStride = height;
    config.encoderConfig.inputFormat = PixelFormat::NV12;
    config.encoderConfig.fps = kCameraFps;
    config.encoderConfig.bitratePreset = MppBitratePreset::Medium;
    config.encoderConfig.gop = kCameraFps;
    return config;
}

Live555RtspServer::StreamConfig makeRtspStreamConfig(
    const std::string& streamName,
    VideoCodec codec,
    CamManager& cameraManager,
    int cameraId,
    const std::shared_ptr<RtspPublishSink>& publishSink)
{
    Live555RtspServer::StreamConfig config {};
    config.streamName = streamName;
    config.codec = codec;
    config.onClientActiveChanged = [&cameraManager, cameraId, publishSink, streamName](bool active) {
        // 该回调在 live555 事件线程执行；两个接口都只投递内部命令，不直接做 V4L2/MPP 重活。
        publishSink->setActive(active);
        const bool accepted = active ? cameraManager.startCamera(cameraId)
                                     : cameraManager.stopCamera(cameraId);
        if (!accepted) {
            LOG_ERROR("IpcApp", "stream=" << streamName
                                               << (active ? " 启动" : " 停止")
                                               << "相机命令投递失败: " << cameraManager.lastError());
        }
    };
    return config;
}

} // namespace

int main()
{
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    CamManager cameraManager;
    Live555RtspServer rtspServer;

    const int mainCameraId = cameraManager.addCamera(makeCameraConfig(kMainDevicePath, 1920, 1080));
    if (mainCameraId < 0) {
        LOG_ERROR("IpcApp", "创建 main VPSS Camera 失败: " << cameraManager.lastError());
        return 1;
    }

    const int subCameraId = cameraManager.addCamera(makeCameraConfig(kSubDevicePath, 1280, 720));
    if (subCameraId < 0) {
        LOG_ERROR("IpcApp", "创建 sub VPSS Camera 失败: " << cameraManager.lastError());
        return 1;
    }

    const auto mainPublishSink = std::make_shared<RtspPublishSink>(
        rtspServer, makePublishConfig("main", VideoCodec::H265, 1920, 1080));
    const auto subPublishSink = std::make_shared<RtspPublishSink>(
        rtspServer, makePublishConfig("sub", VideoCodec::H264, 1280, 720));

    if (!cameraManager.addFrameSink(mainCameraId, mainPublishSink) ||
        !cameraManager.addFrameSink(subCameraId, subPublishSink)) {
        LOG_ERROR("IpcApp", "添加 RTSP 发布 Sink 失败: " << cameraManager.lastError());
        return 1;
    }

    // 先起 poll 线程，确保 live555 PLAY 回调投递的 startCamera()/stopCamera() 能立即被执行。
    cameraManager.startPolling();

    if (!rtspServer.addStream(makeRtspStreamConfig("main", VideoCodec::H265,
                                                   cameraManager, mainCameraId, mainPublishSink)) ||
        !rtspServer.addStream(makeRtspStreamConfig("sub", VideoCodec::H264,
                                                   cameraManager, subCameraId, subPublishSink))) {
        LOG_ERROR("IpcApp", "注册 RTSP stream 失败: " << rtspServer.lastError());
        cameraManager.shutdownPolling();
        return 1;
    }

    // 空用户名/密码表示不启用认证。
    if (!rtspServer.start(kRtspPort, "", "")) {
        LOG_ERROR("IpcApp", "启动 RTSP Server 失败: " << rtspServer.lastError());
        cameraManager.shutdownPolling();
        return 1;
    }

    LOG_INFO("IpcApp", "IPC RTSP 服务已就绪（无客户端时两路 VPSS 均停止采集）"
                           << " main=" << rtspServer.rtspURL("main")
                           << " sub=" << rtspServer.rtspURL("sub"));

    while (g_stopRequested == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    LOG_INFO("IpcApp", "收到退出信号，正在停止 IPC RTSP 服务");
    rtspServer.stop();
    mainPublishSink->setActive(false);
    subPublishSink->setActive(false);
    cameraManager.stopAllCameras();
    cameraManager.shutdownPolling();
    return 0;
}
