#pragma once
#include <QLabel>
#include <QPainter>
#include <QTimer>

// The timer exists only while visible work is pending, including on native
// card overlays. Static previews keep their ordinary zero-timer idle behavior.
class LoadingLabel final : public QLabel {
public:
    explicit LoadingLabel(QWidget* parent=nullptr):QLabel(parent) {
        tick.setInterval(80);
        connect(&tick,&QTimer::timeout,this,[this] { angle=(angle+30)%360; update(QRect(3,0,20,height())); });
    }
    void setLoading(bool value) {
        if(busy==value)return;
        busy=value; setProperty("loading",busy); setIndent(busy?19:0);
        if(busy && isVisible())tick.start(); else tick.stop();
        updateGeometry(); update();
    }
protected:
    void showEvent(QShowEvent* event) override { QLabel::showEvent(event); if(busy)tick.start(); }
    void hideEvent(QHideEvent* event) override { tick.stop(); QLabel::hideEvent(event); }
    void paintEvent(QPaintEvent* event) override {
        QLabel::paintEvent(event);
        if(!busy)return;
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#82d9c8"),1.6,Qt::SolidLine,Qt::RoundCap));
        painter.drawArc(QRectF(7,(height()-12)/2.0,12,12),-angle*16,250*16);
    }
private:
    QTimer tick;
    bool busy=false;
    int angle=0;
};
