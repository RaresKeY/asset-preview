#pragma once
#include "viewer.h"
#include <QObject>

// Native scene interface. All calls, callbacks and destruction belong to the
// single render thread. No QWidget, QWindow or GUI-owned GL context crosses it.
class ModelWorker : public QObject, public PreviewView {
public:
    std::function<void(bool,QString)> loaded;
    std::function<void(QImage)> frameReady;
    virtual void viewport(QSize,double)=0;
    virtual void exposed(bool)=0;
    virtual void scroll(double)=0;
    virtual void drag(QPointF,bool)=0;
};
