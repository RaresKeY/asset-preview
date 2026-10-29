#pragma once
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLWindow>

namespace preview {
// Shared/EGL contexts can select GL_NONE for the native drawable. Binding FBO 0
// alone does not select a color buffer; every native paint must do both.
inline void bindColorFramebuffer(QOpenGLContext& context,GLuint framebuffer) {
    auto* gl=context.extraFunctions();
    gl->glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    const GLenum output=framebuffer?GL_COLOR_ATTACHMENT0:GL_BACK;
    gl->glDrawBuffers(1,&output);
}
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
