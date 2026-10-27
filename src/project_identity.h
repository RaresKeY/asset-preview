#pragma once
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <cmath>

// Project mark v1: FNV-1a over the stable namespaced ID; never label/order/status.
inline QIcon projectIcon(const QString& id) {
    quint32 hash=2166136261u;
    for (const auto c:QString("project:"+id)) hash=(hash^c.unicode())*16777619u;
    const char* tones[]={"#ce6c9f","#a495bd","#cfad37","#5596e6","#d8636c","#64b58b","#4aabb8","#cf8755"};
    QPixmap pix(40,40); pix.setDevicePixelRatio(2); pix.fill(Qt::transparent);
    QPainter p(&pix); p.setRenderHint(QPainter::Antialiasing); p.translate(10,10); p.setPen(Qt::NoPen);
    QColor color(tones[(hash/4)%8]); const int shape=hash%4;
    for (int i=0;i<(shape==1?3:shape==2?8:5);++i) {
        p.save(); p.rotate(i*360.0/(shape==1?3:shape==2?8:5)); p.setBrush(i%2?color.lighter(125):color);
        if (shape==0) p.drawEllipse(QRectF(-3,-9,6,9));
        else if (shape==1) p.drawPolygon(QPolygonF({{0,-9},{7,3},{0,1},{-3,-2}}));
        else if (shape==2) p.drawPolygon(QPolygonF({{0,-9},{2,-3},{0,0},{-2,-3}}));
        else p.drawPolygon(QPolygonF({{0,-9},{6,-3},{1,1},{-2,-2}}));
        p.restore();
    }
    return QIcon(pix);
}
