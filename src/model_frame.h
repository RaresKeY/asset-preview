#pragma once
#include <QJsonObject>
#include <QVector3D>

namespace preview {
inline bool frameChanged(const QJsonObject& a, const QJsonObject& b) {
    return a["up_axis"].toString("y") != b["up_axis"].toString("y") ||
        a["front_axis"].toString() != b["front_axis"].toString();
}
struct ModelFrame {
    QVector3D front, right, up;
    explicit ModelFrame(const QJsonObject& settings) {
        const bool zUp=settings["up_axis"].toString("y")=="z";
        up=zUp?QVector3D(0,0,1):QVector3D(0,1,0);
        const auto axis=settings["front_axis"].toString();
        front=zUp?QVector3D(0,-1,0):QVector3D(0,0,1);
        if (!axis.isEmpty()) {
            front={}; const int index=axis.back()==u'x'?0:axis.back()==u'y'?1:2;
            front[index]=axis.startsWith('-')?-1:1;
        }
        // A model's right side is opposite the camera's screen-right vector.
        right=QVector3D::crossProduct(up,front);
        if (!axis.isEmpty()) right=-right;
        else right=QVector3D(1,0,0); // Preserve legacy cardinal conventions.
    }
    QVector3D direction(const QString& view) const {
        if(view=="front") return front;
        if(view=="back") return -front;
        if(view=="right") return right;
        if(view=="left") return -right;
        if(view=="top") return up;
        if(view=="bottom") return -up;
        return (front+right+up).normalized();
    }
};
}
