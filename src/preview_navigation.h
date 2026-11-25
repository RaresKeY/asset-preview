#pragma once
#include <QPointF>
class PreviewView;

namespace preview {
// Additive helpers keep the prepared PreviewView/ModelWorker ABI unchanged.
bool zoomView(PreviewView* view,double factor);
bool panView(PreviewView* view,QPointF viewportDelta);
bool setPanMode(PreviewView* view,bool enabled);
}
