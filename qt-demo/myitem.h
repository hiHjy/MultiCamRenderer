#ifndef MYITEM_H
#define MYITEM_H

#include <QObject>
#include <QQuickItem>
#include <QSGTexture>
#include <QSGSimpleTextureNode>
#include <QSize>
#include <QMutex>
#include <QQueue>

#include <GLES2/gl2.h>
#include <QObject>
#include <QQuickItem>
#include <QImage>
#include <qqmlregistration.h>
#include <QTimer>
#include <thread>
#include <displayConsumer.hpp>

class MyItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
public:
    MyItem();
    ~MyItem();
    std::thread m_camT;

    QSGNode * updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

signals:
    void displayFrameDone(int bufferIndex);

public slots:
    void setFrame(DisplayFrame frame);

private:
    DisplayFrame m_latestFrame;
    QMutex m_frameMutex;
    bool m_hasFrame = false;
    int m_displayedBufferIndex = -1;
    QQueue<int> m_retiredDisplayedIndexes;
};

#endif // MYITEM_H
