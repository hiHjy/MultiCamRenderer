#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QDebug>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QDebug>
int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    // 直接从 Qt 资源系统加载（QML 编译进二进制，无需外部文件）
    engine.load(QUrl("qrc:/QtDemo/Main.qml"));
      // 获取根窗口
    QList<QObject *> roots = engine.rootObjects();
    if (!roots.isEmpty()) {
        QQuickWindow *window = qobject_cast<QQuickWindow *>(roots.first());
        if (window) {
            qDebug() << "Window size:" << window->size();
        }
    }
    return app.exec();
}
