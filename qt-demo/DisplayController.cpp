#include "DisplayController.hpp"
#include "AppRuntime.hpp"
#include "CamManager.hpp"
#include "Log.hpp"

DisplayController::DisplayController(QObject* parent)
	: QObject(parent)
{
	camMgr = &AppRuntime::getInstance().getCamManager();
	camMgr->startPolling();
}

int DisplayController::addLocalCam(const QString& path)
{
	CamManager::CameraConfig config{};
	config.bufferCount = 4;
	config.devicePath = path.toStdString();
	config.width = 640;
	config.height = 480;
	config.fps = 30;
	config.format = PixelFormat::YUYV;

	const int cameraId = camMgr->addCamera(config);
	if (cameraId < 0) {
		LOG_ERROR("DisplayController", "addCamera failed: " << camMgr->lastError());
		return -1;
	}

	return cameraId;
}

bool DisplayController::startLocalCam(int cameraId)
{
	return camMgr->startCamera(cameraId);
}
