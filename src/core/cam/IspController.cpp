#include "IspController.hpp"

#include "Log.hpp"

// AIQ SDK 的类型不应泄漏到公共头文件，避免上层依赖 vendor 定义。
#include <uAPI2/rk_aiq_user_api2_sysctl.h>

namespace {

constexpr int kIspCameraId = 0;
constexpr char kIspIqFilesPath[] = "/oem/usr/share/iqfiles";

XCamReturn aiqErrorCallback(rk_aiq_err_msg_t* message)
{
    if (message != nullptr) {
        LOG_ERROR("IspController", "AIQ 内部错误 code=" << message->err_code);
    }
    return XCAM_RETURN_NO_ERROR;
}

} // namespace

IspController::~IspController()
{
    stop();
}

bool IspController::start()
{
    rk_aiq_static_info_t staticInfo {};
    if (rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(kIspCameraId, &staticInfo) !=
        XCAM_RETURN_NO_ERROR) {
        LOG_ERROR("IspController", "枚举 ISP sensor 元数据失败 cameraId=" << kIspCameraId);
        return false;
    }

    const char* const sensorName = staticInfo.sensor_info.sensor_name;
    if (staticInfo.sensor_info.phyId < 0 || sensorName == nullptr || sensorName[0] == '\0') {
        LOG_ERROR("IspController", "未找到可用 ISP sensor cameraId=" << kIspCameraId);
        return false;
    }

    if (rk_aiq_uapi2_sysctl_preInit_scene(sensorName, "normal", "day") !=
        XCAM_RETURN_NO_ERROR) {
        LOG_ERROR("IspController", "AIQ 预设 normal/day 场景失败 sensor=" << sensorName);
        return false;
    }

    rk_aiq_sys_ctx_t* context =
        rk_aiq_uapi2_sysctl_init(sensorName, kIspIqFilesPath, aiqErrorCallback, nullptr);
    if (context == nullptr) {
        LOG_ERROR("IspController", "AIQ 初始化失败 sensor=" << sensorName
                                                               << " iqFiles=" << kIspIqFilesPath);
        return false;
    }
    m_context = context;

    if (rk_aiq_uapi2_sysctl_prepare(context, 0, 0, RK_AIQ_WORKING_MODE_NORMAL) !=
        XCAM_RETURN_NO_ERROR) {
        LOG_ERROR("IspController", "AIQ prepare 失败");
        stop();
        return false;
    }

    if (rk_aiq_uapi2_sysctl_start(context) != XCAM_RETURN_NO_ERROR) {
        LOG_ERROR("IspController", "启动 AIQ 3A 失败");
        stop();
        return false;
    }
    m_aiqStarted = true;

    LOG_INFO("IspController", "ISP/AIQ 已由 ipc_app 接管 sensor=" << sensorName
                                                                      << " iqFiles=" << kIspIqFilesPath);
    return true;
}

void IspController::stop()
{
    if (m_context != nullptr) {
        auto* const context = static_cast<rk_aiq_sys_ctx_t*>(m_context);
        if (m_aiqStarted && rk_aiq_uapi2_sysctl_stop(context, false) !=
                               XCAM_RETURN_NO_ERROR) {
            LOG_WARN("IspController", "停止 AIQ 3A 返回失败");
        }
        rk_aiq_uapi2_sysctl_deinit(context);
        m_context = nullptr;
        m_aiqStarted = false;
    }
}
