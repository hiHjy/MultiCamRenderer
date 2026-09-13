#include "CamManager.hpp"
#include "IspController.hpp"
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
    const std::shared_ptr<RtspPublishSink>& publishSink)
{
    Live555RtspServer::StreamConfig config {};
    config.streamName = streamName;
    config.codec = codec;
    config.onClientActiveChanged = [publishSink, streamName](bool active) {
        // 该回调在 live555 事件线程执行，只切换本路编码器状态，不做 V4L2/MPP 重活。
        //
        // VPSS 的 V4L2 采集必须保持 STREAMON：AIQ 的 AE/AWB/AF 依赖连续的 sensor
        // 帧时钟。无人观看时让相机 STREAMOFF 会打断 3A 状态，重新 PLAY 后可能出现
        // 黑帧或异常色彩。因此无客户端时由 PublishSink 丢弃裸帧、停止 MPP 编码器，
        // 而不是停止 CamManager。
        publishSink->setActive(active);
        LOG_INFO("IpcApp", "stream=" << streamName
                                        << (active ? " 有客户端，启动编码" : " 无客户端，停止编码并丢弃裸帧"));
    };
    config.onAdditionalClientStarted = [publishSink, streamName] {
        // 此回调来自 live555 事件线程；PublishSink 只置位标志，MPP 控制命令由编码
        // worker 在下一次送帧前串行执行。首个客户端会新建编码器，天然从 IDR 起流。
        publishSink->requestKeyFrame();
        LOG_INFO("IpcApp", "stream=" << streamName << " 新客户端加入，已请求下一帧 IDR");
    };
    return config;
}

} // namespace

int main()
{
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    IspController ispController;
    if (!ispController.start()) {
        return 1;
    }

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

    // 先起 poll 线程并启动两路 VPSS。即使无 RTSP 客户端也保持 V4L2 STREAMON，
    // 使板端 AIQ 的 3A 持续收敛；无客户端时 PublishSink 不接收/编码这些帧。
    cameraManager.startPolling();
    if (!cameraManager.startAllCameras()) {
        LOG_ERROR("IpcApp", "启动 VPSS Camera 失败: " << cameraManager.lastError());
        // startAllCameras() 可能已成功启动前一路，必须先逐路 STREAMOFF，
        // 再退出 poll 线程，最后才允许 IspController 析构停止 AIQ。
        cameraManager.stopAllCameras();
        cameraManager.shutdownPolling();
        return 1;
    }

    if (!rtspServer.addStream(makeRtspStreamConfig("main", VideoCodec::H265, mainPublishSink)) ||
        !rtspServer.addStream(makeRtspStreamConfig("sub", VideoCodec::H264, subPublishSink))) {
        LOG_ERROR("IpcApp", "注册 RTSP stream 失败: " << rtspServer.lastError());
        cameraManager.stopAllCameras();
        cameraManager.shutdownPolling();
        return 1;
    }

    // 空用户名/密码表示不启用认证。
    if (!rtspServer.start(kRtspPort, "", "")) {
        LOG_ERROR("IpcApp", "启动 RTSP Server 失败: " << rtspServer.lastError());
        cameraManager.stopAllCameras();
        cameraManager.shutdownPolling();
        return 1;
    }

    LOG_INFO("IpcApp", "IPC RTSP 服务已就绪（两路 VPSS 保持采集；无客户端时不编码）"
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
