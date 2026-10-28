#pragma once
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLWindow>

namespace preview {
// Wayland does not guarantee readable native back-buffer contents between
// frames. Allocate a target only for an explicit capture, and release it while
// its context is still current. Ordinary rendering retains no capture buffer.
template<class Paint>
QImage captureFramebuffer(QOpenGLWindow& window,Paint paint) {
    if(!window.isValid()) return {};
    window.makeCurrent();
    QImage image;
    {
        QOpenGLFramebufferObject target(window.size()*window.devicePixelRatio(),
                                       QOpenGLFramebufferObject::CombinedDepthStencil);
        if(target.isValid() && target.bind()) {
            paint(target.handle());
            image=target.toImage().convertToFormat(QImage::Format_RGB32);
        }
    }
    window.doneCurrent();
    return image;
}
}
