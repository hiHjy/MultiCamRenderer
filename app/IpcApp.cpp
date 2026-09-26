#include "CamManager.hpp"
#include "AudioDiagnosticCapture.hpp"
#include "AudioPipeline.hpp"
#include "IspController.hpp"
#include "Live555RtspServer.hh"
#include "Log.hpp"
#include "OnvifServer.hpp"
#include "RtspAudioPublishSink.hpp"
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
constexpr unsigned short kOnvifDeviceServicePort = 8899;
// 第一版固定管理员账号。后续接入 ONVIF CreateUsers/SetUser 或本机配置文件时，
// 只替换这组凭据的来源；RTSP 与 ONVIF 必须始终使用同一套账号。
constexpr char kAdministratorUsername[] = "admin";
constexpr char kAdministratorPassword[] = "admin";
constexpr int kCameraFps = 30;
constexpr int kCameraBufferCount = 6;
// 字体是系统运行资源，不嵌进程序或项目源码。量产 rootfs 需在此路径部署一份
// 覆盖英文/数字的 TTF；以后要显示中文时换成支持中文的字体即可。
constexpr char kOsdFontPath[] = "/oem/usr/share/fonts/DejaVuSans.ttf";

volatile std::sig_atomic_t g_stopRequested = 0;
volatile std::sig_atomic_t g_audioSnapshotRequested = 0;

extern "C" void handleSignal(int signalNumber)
{
    if (signalNumber == SIGUSR1) {
        // signal handler 只能置位；订阅、分配内存、写文件都由 main 线程完成。
        g_audioSnapshotRequested = 1;
    } else {
        g_stopRequested = 1;
    }
}

} // namespace

int main()
{
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::signal(SIGUSR1, handleSignal);

    IspController ispController;
    if (!ispController.start()) {
        return 1;
    }

    CamManager cameraManager;
    Live555RtspServer rtspServer;
    OnvifServer onvifServer;
    AudioPipeline audioPipeline;
    AudioDiagnosticCapture audioDiagnosticCapture;

    // 音频采集与 AAC-LC 编码独立于 main/sub 视频分辨率：整台 IPC 只采集、编码一次，
    // Server 再将同一份不可变 AAC 包分发到两个 URL 的独立 audio track queue。
    if (!audioPipeline.startCapture()) {
        LOG_ERROR("IpcApp", "启动音频采集失败: " << audioPipeline.lastError());
        return 1;
    }
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

    /*
     * 这一个 Sink 被 main/sub 共同使用：首个 audio client 到来时才在自己的 worker 中订阅
     * AAC 编码；全部 audio client 离开后自动退订。App 不填写 PCM、1024 samples 或 bit/s。
     */
    auto audioPublishSink = std::make_shared<RtspAudioPublishSink>(
        audioPipeline, rtspServer,
        RtspAudioPublishSink::Config {AUDIO_CODEC_AAC, AudioBitratePreset::Medium});
    if (!audioPublishSink->isReady()) {
        LOG_ERROR("IpcApp", "创建 RTSP 音频发布 Sink 失败: " << audioPublishSink->lastError());
        cameraManager.stopAllCameras();
        cameraManager.shutdownPolling();
        audioPipeline.stopCapture();
        return 1;
    }

    Live555RtspServer::StreamConfig mainRtspConfig {};
    mainRtspConfig.streamName = "main";
    mainRtspConfig.codec = VideoCodec::H265;
    mainRtspConfig.audio.enabled = true;
    mainRtspConfig.audio.encoded = audioPublishSink->streamInfo();
    Live555RtspServer::StreamConfig subRtspConfig {};
    subRtspConfig.streamName = "sub";
    subRtspConfig.codec = VideoCodec::H264;
    subRtspConfig.audio = mainRtspConfig.audio;
    if (!rtspServer.addStream(mainRtspConfig, mainPublishSink, audioPublishSink) ||
        !rtspServer.addStream(subRtspConfig, subPublishSink, audioPublishSink)) {
        LOG_ERROR("IpcApp", "注册 RTSP stream 失败: " << rtspServer.lastError());
        cameraManager.stopAllCameras();
        cameraManager.shutdownPolling();
        audioPipeline.stopCapture();
        return 1;
    }

    if (!rtspServer.start(kRtspPort, kAdministratorUsername, kAdministratorPassword)) {
        LOG_ERROR("IpcApp", "启动 RTSP Server 失败: " << rtspServer.lastError());
        audioPublishSink.reset();
        audioPipeline.stopCapture();
        cameraManager.stopAllCameras();
        cameraManager.shutdownPolling();
        return 1;
    }

    // IpcApp 只提供自身的媒体资源；IP/MAC 选择、DHCP 换址重启和 Discovery 生命周期
    // 都是 OnvifServer 的内部职责，避免把协议细节散落在应用主循环。
    OnvifServerConfig onvifConfig;
    onvifConfig.deviceServicePort = kOnvifDeviceServicePort;
    onvifConfig.rtspPort = kRtspPort;
    onvifConfig.username = kAdministratorUsername;
    onvifConfig.password = kAdministratorPassword;
    if (!onvifServer.start(onvifConfig)) {
        LOG_WARN("IpcApp", "ONVIF 配置非法，未启动: " << onvifServer.lastError());
    } else if (onvifServer.isRunning()) {
        LOG_INFO("IpcApp", "ONVIF Device/Media Service 已就绪 url=" << onvifServer.deviceServiceUrl());
    } else {
        LOG_WARN("IpcApp", "ONVIF 正等待可用网络: " << onvifServer.lastError());
    }

    LOG_INFO("IpcApp", "IPC RTSP 服务已就绪（两路 VPSS 保持采集；无视频客户端时不编码；"
                           "AAC-LC 在 main/sub 间共用一份编码）"
                           << " main=" << rtspServer.rtspURL("main")
                           << " sub=" << rtspServer.rtspURL("sub"));

    /* 音频采集发生 XRUN 时可能成功恢复，也可能线程最终退出。前者至少留下一条
       运行期证据，后者必须明确报错；不能让 RTSP 仍活着、音频却静默消失。 */
    AudioCaptureStatistics lastAudioCaptureStatistics = audioPipeline.captureStatistics();
    while (g_stopRequested == 0) {
        if (g_audioSnapshotRequested != 0) {
            g_audioSnapshotRequested = 0;
            if (!audioDiagnosticCapture.startOneShot(audioPipeline)) {
                LOG_WARN("IpcApp", "忽略 SIGUSR1 音频快照请求: "
                                       << audioDiagnosticCapture.lastError());
            }
        }

        const AudioCaptureStatistics audioStatistics = audioPipeline.captureStatistics();
        const bool recoveredChanged =
            audioStatistics.xrunCount != lastAudioCaptureStatistics.xrunCount ||
            audioStatistics.recoveredErrorCount != lastAudioCaptureStatistics.recoveredErrorCount;
        const bool captureStopped = lastAudioCaptureStatistics.running != 0 &&
                                    audioStatistics.running == 0;
        const bool fatalChanged =
            audioStatistics.fatalErrorCount != lastAudioCaptureStatistics.fatalErrorCount;
        if (captureStopped || fatalChanged) {
            LOG_ERROR("IpcApp", "音频采集已异常停止"
                                     << " xrun=" << audioStatistics.xrunCount
                                     << " recovered=" << audioStatistics.recoveredErrorCount
                                     << " fatal=" << audioStatistics.fatalErrorCount
                                     << " lastAlsaError=" << audioStatistics.lastError);
        } else if (recoveredChanged) {
            LOG_WARN("IpcApp", "音频采集出现并已恢复的 ALSA 异常"
                                    << " xrun=" << audioStatistics.xrunCount
                                    << " recovered=" << audioStatistics.recoveredErrorCount
                                    << " lastAlsaError=" << audioStatistics.lastError);
        }
        lastAudioCaptureStatistics = audioStatistics;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("IpcApp", "收到退出信号，正在停止 IPC RTSP 服务");
    audioDiagnosticCapture.cancel();
    onvifServer.stop();
    rtspServer.stop();
    audioPublishSink.reset();
    audioPipeline.stopCapture();
    cameraManager.stopAllCameras();
    cameraManager.shutdownPolling();
    return 0;
}
