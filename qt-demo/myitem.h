#ifndef MYITEM_H
#define MYITEM_H

#include <QObject>
#include <QMutex>
#include <QQueue>
#include <QQuickItem>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSize>

#include <GLES2/gl2.h>
#include <qqmlregistration.h>

#include "displaySink.hpp"


// 后期如果要把“延迟一帧 release buffer”升级成更明确的 Qt 渲染同步，
// 可以研究 QQuickWindow 的这几个信号：
//
// 1. QQuickWindow::afterRendering()
//    Qt Scene Graph 完成本窗口当前帧的渲染命令提交后触发。
//    触发时还在渲染流程里，OpenGL/EGL 上下文通常仍然是当前上下文。
//    适合做少量和 GL 状态相关的事情，但不建议在这里做重活或阻塞等待。
//
// 2. QQuickWindow::afterFrameEnd()
//    Qt 6 提供的帧结束信号，表示这一帧的 Scene Graph 渲染流程已经走到末尾。
//    从语义上看，它比 afterRendering 更适合做“这一帧结束后的清理/归还记录”。
//    后面如果要在 Qt 侧统一归还显示 buffer，可以优先考虑这个点。
//
// 3. QQuickWindow::frameSwapped()
//    后端完成 swap/present 后触发，传统 OpenGL swap buffers 之后会收到。
//    对屏幕显示完成更直观，但具体时机受平台后端影响；EGLFS/KMS 下要实测。
//
// 当前第一版仍使用 m_retiredDisplayedIndexes 延迟一帧 release：
// updatePaintNode() setTexture 后不立刻归还旧 buffer，避免 Qt/GPU 还没采样完，
// DisplaySink/RGA 就把同一块 dma-buf 重新写成下一帧导致闪烁或撕裂。
class MyItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
public:
    MyItem();
    ~MyItem();

    // Qt Scene Graph 真正要重绘这个 Item 时会调用 updatePaintNode。
    // setFrame() 只保存最新帧并调用 update()，真正的 dmaFd -> texture 导入在这里发生。
    QSGNode * updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

signals:
    void displayFrameDone(int bufferIndex);

public slots:
    // DisplaySink 把 RGA 转好的 RGBA dma-buf 通过这个槽送进 Qt。
    // 这里不能直接画，只能记住最新帧，然后 update() 请求下一轮 Scene Graph 重绘。
    void setFrame(DisplayFrame frame);

private:
    // 最新待显示帧。DisplayFrame 只保存 fd/宽高/stride/index 这些标量信息，
    // 真正 DMA buffer 的所有权还在 DisplaySink 的 pool 里。
    std::shared_ptr<DisplaySink> m_displaySink = std::make_shared<DisplaySink>();
    DisplayFrame m_latestFrame;
    QMutex m_frameMutex;
    bool m_hasFrame = false;
    int m_displayedBufferIndex = -1;
    QQueue<int> m_retiredDisplayedIndexes;
};

#endif // MYITEM_H
