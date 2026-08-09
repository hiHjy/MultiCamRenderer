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

        auto it = m_textures.find(frame.dmaFd);
        if (it != m_textures.end())
            return it.value();

        EGLDisplay eglDisplay = eglGetCurrentDisplay();
        if (eglDisplay == EGL_NO_DISPLAY) {
            qWarning() << "eglGetCurrentDisplay failed";
            return nullptr;
        }

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

        const EGLint attrs[] = {
            EGL_WIDTH, frame.width,
            EGL_HEIGHT, frame.height,
            EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(DRM_FORMAT_ABGR8888),
            EGL_DMA_BUF_PLANE0_FD_EXT, frame.dmaFd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, frame.stride * 4,
            EGL_NONE,
        };

        EGLImageKHR eglImage = eglCreateImageKHRFunc(
            eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
        if (eglImage == EGL_NO_IMAGE_KHR) {
            qWarning() << "eglCreateImageKHR failed, eglError:" << Qt::hex << eglGetError();
            return nullptr;
        }

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
        glEGLImageTargetTexture2DOESFunc(GL_TEXTURE_2D, reinterpret_cast<GLeglImageOES>(eglImage));
        glBindTexture(GL_TEXTURE_2D, 0);

        eglDestroyImageKHRFunc(eglDisplay, eglImage);

        QSGTexture *texture = QNativeInterface::QSGOpenGLTexture::fromNative(
            textureId,
            window,
            QSize(frame.width, frame.height),
            QQuickWindow::CreateTextureOptions() | QQuickWindow::TextureOwnsGLTexture);
        if (!texture) {
            glDeleteTextures(1, &textureId);
            return nullptr;
        }

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

    QSGTexture *texture = node->textureForFrame(window(), frame);
    if (!texture)
        return node;

    node->setTexture(texture);
    node->setOwnsTexture(false);
    node->setRect(boundingRect());

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
        if (m_hasFrame && m_latestFrame.bufferIndex != m_displayedBufferIndex &&
            m_latestFrame.bufferIndex != frame.bufferIndex) {
            droppedIndex = m_latestFrame.bufferIndex;
        }

        m_latestFrame = frame;
        m_hasFrame = true;
    }

    if (droppedIndex >= 0)
        emit displayFrameDone(droppedIndex);

    update();
}
