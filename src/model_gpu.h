#pragma once
#include <QMutex>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSize>
#include <QWindow>
#include <memory>

// The context belongs to the render thread. Imports use the offscreen surface;
// display rendering uses the native window under the surface lifetime lock.
struct ModelGpuContext {
    QOpenGLContext* context=nullptr;
    QOffscreenSurface* surface=nullptr;
    QMutex mutex{};
    QWindow* window=nullptr;
    bool exposed=false;
};
