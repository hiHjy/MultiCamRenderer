#include "AppRuntime.hpp"
#include "Log.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    // AppRuntime& runtime = AppRuntime::getInstance();
    // CamManager& camManager = runtime.getCamManager();

    // CamManager::CameraConfig config{};
    // config.width = 640;
    // config.height = 480;
    // config.fps = 30;
    // config.devicePath = "/dev/video10";
    // config.format = PixelFormat::YUYV;

    // const int cameraId = camManager.addCamera(config);
    // if (cameraId < 0) {
    //     LOG_ERROR("QtDemo", "addCamera failed: " << camManager.lastError());
    //     return -1;
    // }

    // QObject::connect(&app, &QCoreApplication::aboutToQuit, [&camManager] {
    //     camManager.shutdownPolling();
    // });

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    // 直接从 Qt 资源系统加载（QML 编译进二进制，无需外部文件）
    engine.load(QUrl("qrc:/QtDemo/Main.qml"));

    // const QList<QObject*> roots = engine.rootObjects();
    // if (!roots.isEmpty()) {
    //     QQuickWindow* window = qobject_cast<QQuickWindow*>(roots.first());
    //     if (window) {
    //         LOG_INFO("QtDemo", "window size=" << window->width() << "x" << window->height());
    //     }
    // }

    // if (!camManager.startAllCameras()) {
    //     LOG_ERROR("QtDemo", "startAllCameras failed: " << camManager.lastError());
    //     return -1;
    // }

    // camManager.startPolling();
    return app.exec();
}
