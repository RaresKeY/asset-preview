#pragma once
#include <QElapsedTimer>
#include <QLabel>
#include <QPainter>
#include <QTimer>

// The timer exists only while visible work is pending, including on native
// card overlays. Static previews keep their ordinary zero-timer idle behavior.
class LoadingLabel final : public QLabel {
public:
    explicit LoadingLabel(QWidget* parent=nullptr):QLabel(parent) {
        tick.setTimerType(Qt::PreciseTimer); tick.setInterval(16);
        connect(&tick,&QTimer::timeout,this,[this] { update(QRect(3,0,20,height())); });
    }
    void setLoading(bool value) {
        if(busy==value)return;
        busy=value; setProperty("loading",busy); setIndent(busy?19:0);
        if(busy && isVisible())start(); else tick.stop();
        updateGeometry(); update();
    }
protected:
    void showEvent(QShowEvent* event) override { QLabel::showEvent(event); if(busy)start(); }
    void hideEvent(QHideEvent* event) override { tick.stop(); QLabel::hideEvent(event); }
    void paintEvent(QPaintEvent* event) override {
        QLabel::paintEvent(event);
        if(!busy)return;
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#82d9c8"),1.6,Qt::SolidLine,Qt::RoundCap));
        const auto elapsed=clock.isValid()?clock.elapsed():0;
        painter.drawArc(QRectF(7,(height()-12)/2.0,12,12),-int(elapsed%960)*360*16/960,250*16);
    }
private:
    QTimer tick;
    QElapsedTimer clock;
    bool busy=false;
    void start() { clock.start(); tick.start(); }
};
