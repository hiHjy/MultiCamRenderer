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
// 字体是系统运行资源，不嵌进程序或项目源码。量产 rootfs 需在此路径部署一份
// 覆盖英文/数字的 TTF；以后要显示中文时换成支持中文的字体即可。
constexpr char kOsdFontPath[] = "/oem/usr/share/fonts/DejaVuSans.ttf";

volatile std::sig_atomic_t g_stopRequested = 0;

extern "C" void handleSignal(int)
{
    g_stopRequested = 1;
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

    CamManager::CameraConfig mainCameraConfig {};
    mainCameraConfig.devicePath = kMainDevicePath;
    mainCameraConfig.width = 1920;
    mainCameraConfig.height = 1080;
    mainCameraConfig.fps = kCameraFps;
    mainCameraConfig.format = PixelFormat::NV12;
    // 编码 worker 最多持有一张、待编码队列最多一张，6 块留出采集余量。
    mainCameraConfig.bufferCount = kCameraBufferCount;
    const int mainCameraId = cameraManager.addCamera(mainCameraConfig);
    if (mainCameraId < 0) {
        LOG_ERROR("IpcApp", "创建 main VPSS Camera 失败: " << cameraManager.lastError());
        return 1;
    }

    CamManager::CameraConfig subCameraConfig = mainCameraConfig;
    subCameraConfig.devicePath = kSubDevicePath;
    subCameraConfig.width = 1280;
    subCameraConfig.height = 720;
    const int subCameraId = cameraManager.addCamera(subCameraConfig);
    if (subCameraId < 0) {
        LOG_ERROR("IpcApp", "创建 sub VPSS Camera 失败: " << cameraManager.lastError());
        return 1;
    }

    RtspPublishSink::Config mainPublishConfig {};
    mainPublishConfig.streamName = "main";
    mainPublishConfig.encoderConfig.codec = MppCodec::H265;
    mainPublishConfig.encoderConfig.fps = kCameraFps;
    mainPublishConfig.encoderConfig.bitratePreset = MppBitratePreset::Medium;
    mainPublishConfig.encoderConfig.gop = kCameraFps;
    mainPublishConfig.enableOsd = true;
    mainPublishConfig.osdConfig.fontPath = kOsdFontPath;
    // IPC 的 OSD 样式以主码流 1080p 设计；OsdRenderer 会按实际 VPSS 帧比例自动缩放，
    // 因而 sub 720p 使用约 2/3 的字号、边距、padding 与检测框线宽。
    mainPublishConfig.osdConfig.designWidth = 1920;
    mainPublishConfig.osdConfig.designHeight = 1080;

    RtspPublishSink::Config subPublishConfig = mainPublishConfig;
    subPublishConfig.streamName = "sub";
    subPublishConfig.encoderConfig.codec = MppCodec::H264;

    const auto mainPublishSink = std::make_shared<RtspPublishSink>(rtspServer, mainPublishConfig);
    const auto subPublishSink = std::make_shared<RtspPublishSink>(rtspServer, subPublishConfig);

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

    Live555RtspServer::StreamConfig mainRtspConfig {};
    mainRtspConfig.streamName = "main";
    mainRtspConfig.codec = VideoCodec::H265;
    Live555RtspServer::StreamConfig subRtspConfig {};
    subRtspConfig.streamName = "sub";
    subRtspConfig.codec = VideoCodec::H264;
    if (!rtspServer.addStream(mainRtspConfig, mainPublishSink) ||
        !rtspServer.addStream(subRtspConfig, subPublishSink)) {
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
    cameraManager.stopAllCameras();
    cameraManager.shutdownPolling();
    return 0;
}
