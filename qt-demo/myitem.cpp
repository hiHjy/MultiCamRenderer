#include "myitem.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm/drm_fourcc.h>

#include <QDebug>
#include <QHash>
#include <QMetaType>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>

#include <CamManager.hpp>

namespace {

// QQuickItem 不能直接拿 dma-buf 画图。
// 我们这里用一个 QSGSimpleTextureNode 表示“贴了一张纹理的矩形”：
//   DisplayFrame.dmaFd
//        -> EGLImage
//        -> OpenGL texture id
//        -> QSGTexture
//        -> QSGSimpleTextureNode
// Qt Scene Graph 最后会把这个 node 画到 MyItem 对应的 QML 区域上。
class DmaTextureNode final : public QSGSimpleTextureNode {
public:
    ~DmaTextureNode() override
    {
        qDeleteAll(m_textures);
    }

    QSGTexture *textureForFrame(QQuickWindow *window, const DisplayFrame &frame)
    {
        if (!window || frame.dmaFd < 0 || frame.width <= 0 || frame.height <= 0)
            return nullptr;

        // DisplayConsumer 的显示 pool 是固定几块 DMA buffer 循环使用。
        // 同一个 dmaFd 会反复出现，所以第一次导入成纹理后缓存起来。
        // 这样每帧只换内容，不重复创建/销毁 EGLImage、GL texture、QSGTexture。
        auto it = m_textures.find(frame.dmaFd);
        if (it != m_textures.end())
            return it.value();

        // updatePaintNode 运行在 Qt Scene Graph 渲染线程，此时当前线程已经有 EGL/GL 上下文。
        // eglGetCurrentDisplay() 拿到的就是 Qt 正在用的 EGL display。
        EGLDisplay eglDisplay = eglGetCurrentDisplay();
        if (eglDisplay == EGL_NO_DISPLAY) {
            qWarning() << "eglGetCurrentDisplay failed";
            return nullptr;
        }

        // 这几个函数不是普通 GL ES 头文件里的固定符号，要通过 eglGetProcAddress 获取。
        // eglCreateImageKHR: 把 Linux dma-buf fd 包装成 EGLImage。
        // glEGLImageTargetTexture2DOES: 把 EGLImage 绑定到 OpenGL texture 上。
        PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHRFunc =
            reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHRFunc =
            reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOESFunc =
            reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));

        if (!eglCreateImageKHRFunc || !eglDestroyImageKHRFunc || !glEGLImageTargetTexture2DOESFunc) {
            qWarning() << "EGL dmabuf import functions unavailable";
            return nullptr;
        }

        // 告诉 EGL 这块 dma-buf 的图像布局：
        // - dmaFd 是底层 DMA buffer 句柄。
        // - DRM_FORMAT_ABGR8888 要和 RGA 写入的 RGBA 布局匹配。
        // - PITCH 是一行占多少字节。VideoFrame::stride 单位是像素，
        //   RGBA8888 一像素 4 字节，所以这里是 stride * 4。
        // 这一步还没有创建 OpenGL 纹理，只是让 EGL 认识这块 dma-buf。
        const EGLint attrs[] = {
            EGL_WIDTH, frame.width,
            EGL_HEIGHT, frame.height,
            EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(DRM_FORMAT_ABGR8888),
            EGL_DMA_BUF_PLANE0_FD_EXT, frame.dmaFd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, frame.stride * 4,
            EGL_NONE,
        };

        // dma-buf fd -> EGLImage。
        // EGLImage 可以理解成 EGL 对外部图像内存的统一包装。
        EGLImageKHR eglImage = eglCreateImageKHRFunc(
            eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
        if (eglImage == EGL_NO_IMAGE_KHR) {
            qWarning() << "eglCreateImageKHR failed, eglError:" << Qt::hex << eglGetError();
            return nullptr;
        }

        // 先创建一个普通 OpenGL 2D texture id。
        // 后面会把 EGLImage 挂到这个 texture id 上，让 GPU 可以采样 dma-buf。
        GLuint textureId = 0;
        glGenTextures(1, &textureId);
        if (!textureId) {
            qWarning() << "glGenTextures failed";
            eglDestroyImageKHRFunc(eglDisplay, eglImage);
            return nullptr;
        }

        glBindTexture(GL_TEXTURE_2D, textureId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // EGLImage -> OpenGL texture。
        // 这句之后，textureId 背后的数据来源就是 frame.dmaFd 那块 DMA buffer。
        glEGLImageTargetTexture2DOESFunc(GL_TEXTURE_2D, reinterpret_cast<GLeglImageOES>(eglImage));
        glBindTexture(GL_TEXTURE_2D, 0);

        // texture 已经引用了 EGLImage 对应的底层图像，EGLImage 包装对象可以销毁。
        // 真正的 DMA buffer 生命周期仍然由 DisplayConsumer 的 pool 管。
        eglDestroyImageKHRFunc(eglDisplay, eglImage);

        // OpenGL texture id -> Qt Scene Graph 的 QSGTexture。
        // QSGTexture 是 Qt 能放进 QSGSimpleTextureNode 里绘制的纹理对象。
        // TextureOwnsGLTexture 表示 QSGTexture 析构时会释放这个 GL texture id。
        QSGTexture *texture = QNativeInterface::QSGOpenGLTexture::fromNative(
            textureId,
            window,
            QSize(frame.width, frame.height),
            QQuickWindow::CreateTextureOptions() | QQuickWindow::TextureOwnsGLTexture);
        if (!texture) {
            glDeleteTextures(1, &textureId);
            return nullptr;
        }

        // 用 dmaFd 做 key 缓存 QSGTexture，因为同一块显示 buffer 会循环复用。
        m_textures.insert(frame.dmaFd, texture);
        return texture;
    }

private:
    QHash<int, QSGTexture*> m_textures;
};

} // namespace

MyItem::MyItem()
{
    setFlag(ItemHasContents, true);
    qRegisterMetaType<DisplayFrame>("DisplayFrame");

    m_camT = std::thread([this](){
        CamManager *mgr = new CamManager();
        CamManager::CameraConfig config {};
        config.cameraId = 0;
        config.width = 640;
        config.height = 480;
        config.fps = 30;
        config.devicePath = "/dev/video10";
        config.format = PixelFormat::YUYV;
        mgr->addCamera(config);

        auto p = std::make_shared<DisplayConsumer>();
        DisplayConsumer *consumer = p.get();
        mgr->addConsumerForHub(0, p);

        connect(consumer, &DisplayConsumer::frameReady, this, &MyItem::setFrame);
        connect(this, &MyItem::displayFrameDone, consumer, &DisplayConsumer::releaseFrameByIndex,
                Qt::DirectConnection);

        mgr->startAll();
        mgr->run();
    });
}

MyItem::~MyItem()
{
}

QSGNode *MyItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    // Qt 会复用 oldNode。第一次进来没有 node，就创建一个能显示 texture 的节点。
    auto *node = static_cast<DmaTextureNode *>(oldNode);
    if (!node)
        node = new DmaTextureNode();

    DisplayFrame frame;
    int oldDisplayedIndex = -1;
    {
        QMutexLocker lock(&m_frameMutex);
        if (!m_hasFrame)
            return node;

        frame = m_latestFrame;
        oldDisplayedIndex = m_displayedBufferIndex;
    }

    // 这里是 Qt 显示链路最关键的一步：
    // 把 DisplayFrame 里的 dmaFd 导入成 QSGTexture，之后 Scene Graph 就能绘制它。
    QSGTexture *texture = node->textureForFrame(window(), frame);
    if (!texture)
        return node;

    // 把纹理贴到这个 QML Item 对应的矩形区域上。
    // boundingRect() 来自 QML 里 MyItem 的 width/height/x/y。
    node->setTexture(texture);
    node->setOwnsTexture(false);
    node->setRect(boundingRect());

    // 显示侧 buffer 不能刚 setTexture 就立刻 release。
    // Qt Scene Graph 可能还要在这一帧渲染里采样它，所以这里延迟一帧归还旧 buffer。
    if (oldDisplayedIndex != frame.bufferIndex) {
        int releaseIndex = -1;
        {
            QMutexLocker lock(&m_frameMutex);
            m_displayedBufferIndex = frame.bufferIndex;
            if (oldDisplayedIndex >= 0)
                m_retiredDisplayedIndexes.enqueue(oldDisplayedIndex);
            if (m_retiredDisplayedIndexes.size() > 1)
                releaseIndex = m_retiredDisplayedIndexes.dequeue();
        }
        if (releaseIndex >= 0)
            emit displayFrameDone(releaseIndex);
    }

    return node;
}

void MyItem::setFrame(DisplayFrame frame)
{
    int droppedIndex = -1;
    {
        QMutexLocker lock(&m_frameMutex);
        // 如果旧的 latest 还没被 updatePaintNode 接走，新帧又来了，
        // 就丢掉旧 latest，并通知 DisplayConsumer 归还对应 buffer。
        if (m_hasFrame && m_latestFrame.bufferIndex != m_displayedBufferIndex &&
            m_latestFrame.bufferIndex != frame.bufferIndex) {
            droppedIndex = m_latestFrame.bufferIndex;
        }

        m_latestFrame = frame;
        m_hasFrame = true; 
    }

    if (droppedIndex >= 0)
        emit displayFrameDone(droppedIndex);
		
	// 这句话是在告诉 Qt：我这个 QQuickItem 内容变了，下一轮渲染时请重新画我。
    // 真正的绘制不会发生在这里，而是在 Qt 随后调用 updatePaintNode() 时发生。
    update();
}
