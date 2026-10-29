#pragma once
#include <QMutex>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSize>
#include <memory>

// The producer context belongs exclusively to the render thread. Its offscreen
// surface is created/destroyed on the GUI thread. The native display context
// shares GL resources, never the producer's current-context state or FBOs.
struct ModelGpuContext {
    QOpenGLContext* context=nullptr;
    QOffscreenSurface* surface=nullptr;
};
struct ModelGpuFrame {
    QMutex mutex;
    QSize size;
    GLuint texture=0,framebuffer=0;
    GLsync ready=nullptr,consumed=nullptr;
    quint64 serial=0;
};
