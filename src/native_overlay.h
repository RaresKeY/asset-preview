#pragma once
#include <QWidget>

// Native overlays must not promote the surrounding raster UI into native child
// surfaces. Keep their nearest existing native parent and its stacking context.
inline void setNativeOverlay(QWidget* widget,bool enabled=true) {
    widget->setAttribute(Qt::WA_DontCreateNativeAncestors);
    widget->setAttribute(Qt::WA_NativeWindow,enabled);
}
