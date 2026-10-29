#pragma once
#include "viewer.h"
#include "model_gpu.h"
#include <QObject>

// Native scene interface. All calls, callbacks and destruction belong to the
// single render thread. No QWidget, QWindow or GUI-owned GL context crosses it;
// native display shares GPU textures/fences with its own independent GL context.
class ModelWorker : public QObject, public PreviewView {
public:
    std::function<void(bool,QString)> loaded;
    std::function<void(QImage)> frameReady;
    std::function<void(std::shared_ptr<ModelGpuFrame>)> gpuFrameReady;
    virtual void gpuContext(std::shared_ptr<ModelGpuContext>) {}
    virtual void prepare() {}
    virtual bool renderPending() const { return false; }
    virtual void present() { const auto frame=snapshot(); if(frameReady)frameReady(frame); }
    virtual void viewport(QSize,double)=0;
    virtual void exposed(bool)=0;
    virtual void scroll(double)=0;
    virtual void drag(QPointF,bool)=0;
};
