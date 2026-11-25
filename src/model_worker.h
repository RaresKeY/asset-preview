#pragma once
#include "viewer.h"
#include "model_gpu.h"
#include <QObject>

// Native scene interface. All calls, callbacks and destruction belong to the
// single render thread. Only the locked native drawing surface crosses threads;
// window events and native surface creation/destruction stay on the GUI thread.
class ModelWorker : public QObject, public PreviewView {
public:
    std::function<void(bool,QString)> loaded;
    // Frames stay in the renderer. This notification carries metadata only;
    // pixels are available solely through an explicit snapshot request.
    std::function<void()> frameRendered;
    virtual void gpuContext(std::shared_ptr<ModelGpuContext>) {}
    virtual bool renderPending() const { return false; }
    virtual void present()=0;
    virtual void viewport(QSize,double)=0;
    virtual void exposed(bool)=0;
    virtual void scroll(double)=0;
    virtual void pointer(QPointF) {}
    virtual void drag(QPointF,bool)=0;
};
