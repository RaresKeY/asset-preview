#pragma once
#include <QList>
#include <QRect>
#include <algorithm>

inline QRect fitWindowFrame(const QRect& frame,const QList<QRect>& screens) {
    if (screens.isEmpty()) return frame;
    QRect target=screens.first();
    qint64 largest=0;
    for (const auto& screen:screens) {
        if (screen.contains(frame)) return frame;
        const QRect overlap=screen.intersected(frame);
        const qint64 area=qint64(overlap.width())*overlap.height();
        if (area>largest) { largest=area; target=screen; }
    }
    const QSize size=frame.size().boundedTo(target.size());
    return QRect(QPoint(std::clamp(frame.x(),target.left(),target.right()-size.width()+1),
                        std::clamp(frame.y(),target.top(),target.bottom()-size.height()+1)),size);
}
