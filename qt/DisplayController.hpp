#pragma once

#include <QObject>
#include <QString>
#include <qqmlregistration.h>

class CamManager;

class DisplayController : public QObject {
	Q_OBJECT
	QML_ELEMENT

public:
	explicit DisplayController(QObject* parent = nullptr);

	Q_INVOKABLE int addLocalCam(const QString& path);
	Q_INVOKABLE bool startLocalCam(int cameraId);

private:
	CamManager* camMgr = nullptr;
};
