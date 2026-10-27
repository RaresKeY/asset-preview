#pragma once
#include "animation_time.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QSlider>
#include <QSignalBlocker>
#include <functional>
#include <limits>
#include <cmath>

namespace preview {
// A bounded, immediate-mode frame ruler. No pose thumbnails, cache or timer.
// The slider retains native focus/accessibility and commits a drag only once.
class AnimationTimeline final : public QSlider {
public:
    explicit AnimationTimeline(QWidget* parent) : QSlider(Qt::Horizontal,parent) {
        setRange(0,0); setTracking(false); setMinimumWidth(24);
        connect(this,&QSlider::valueChanged,this,[this](int value) {
            position=secondsAt(value);
            if (seekRequested) seekRequested(position);
        });
    }
    std::function<void(double)> seekRequested;
    void cancelDrag() {
        const QSignalBlocker blocker(this);
        setSliderDown(false);
    }
    void setClip(double seconds,int rate) {
        seconds=std::isfinite(seconds)?std::max(0.0,seconds):0;
        rate=std::clamp(rate,1,120);
        if (duration==seconds && fps==rate) return;
        const QSignalBlocker blocker(this);
        cancelDrag(); duration=seconds; fps=rate;
        const double frames=std::ceil(duration*fps);
        setRange(0,int(std::min(frames,double(std::numeric_limits<int>::max()))));
        setSingleStep(1); setPageStep(fps*5);
        setPosition(position); update();
    }
    void setPosition(double seconds) {
        const QSignalBlocker blocker(this);
        const double next=std::isfinite(seconds)?std::clamp(seconds,0.0,duration):0;
        const bool changed=position!=next;
        position=next; setValue(indexAt(position));
        if (changed) update();
    }
    double shownPosition() const { return isSliderDown()?secondsAt(sliderPosition()):position; }
    double secondsAt(int value) const {
        if (value>=maximum()) return duration;
        if (std::ceil(duration*fps)<=maximum()) return std::min(duration,value/double(fps));
        return nearest(duration*(value/double(maximum())));
    }
protected:
    bool event(QEvent* event) override {
        if (event->type()==QEvent::ShortcutOverride) {
            const auto* key=static_cast<QKeyEvent*>(event);
            if (key->modifiers()==Qt::NoModifier) switch (key->key()) {
            case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down:
            case Qt::Key_Home: case Qt::Key_End: case Qt::Key_PageUp: case Qt::Key_PageDown:
                event->accept(); return true;
            }
        }
        return QSlider::event(event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const auto area=QRectF(rect());
        p.fillRect(area,QColor("#1b1e20"));
        const double span=std::max(1,width()-10);
        const double frames=std::min(duration*fps,std::numeric_limits<double>::max());
        auto xAt=[&](double frame) { return 5+span*(frames>0?frame/frames:0); };
        const double shown=shownPosition();
        const double head=5+span*(duration>0?shown/duration:0);
        const QColor accent(isEnabled()?"#3daee9":"#63707d");
        if (height()>20) {
            const int ruler=fontMetrics().height()+10;
            const double labelWidth=std::max(48,fontMetrics().horizontalAdvance(QString::number(std::ceil(frames),'f',0))+18);
            const double needed=std::max(1.0,frames*labelWidth/span);
            const double magnitude=std::pow(10.0,std::floor(std::log10(needed)));
            double interval=magnitude;
            for (double factor:{1.,2.,5.,10.}) if (magnitude*factor>=needed) { interval=magnitude*factor; break; }
            // Tick count is bounded by pixels, even for multi-hour clips.
            for (double frame=0;frame<=frames && frames>0;frame+=interval) {
                const double x=xAt(frame);
                p.setPen(QColor("#31363b")); p.drawLine(QPointF(x,ruler),QPointF(x,height()));
                p.setPen(QColor("#aeb6bc"));
                const QString label=QString::number(frame,'f',0);
                const int w=fontMetrics().horizontalAdvance(label);
                p.drawText(QRectF(std::clamp(x-w/2,2.0,std::max(2.0,double(width()-w-2))),2,w,ruler-4),Qt::AlignCenter,label);
            }
            p.setPen(QColor("#454b50")); p.drawLine(0,ruler,width(),ruler);
            // Individual sample ticks are shown only when distinguishable.
            if (frames>0 && span/frames>=5) {
                p.setPen(QColor("#65727f"));
                for (int frame=0;frame<=frames;++frame)
                    p.drawLine(QPointF(xAt(frame),height()-8),QPointF(xAt(frame),height()-3));
            }
            p.fillRect(QRectF(5,ruler+4,std::max(0.0,head-5),std::max(1,height()-ruler-16)),QColor(61,174,233,25));
            const double frame=shown>=duration?std::ceil(frames):std::floor(animationFrame(shown,fps));
            const QString label=QString::number(frame,'f',0);
            const int w=fontMetrics().horizontalAdvance(label)+12;
            const double left=std::clamp(head-w/2,0.0,std::max(0.0,double(width()-w)));
            p.fillRect(QRectF(left,1,w,ruler-2),accent);
            p.setPen(QColor("#11171d")); p.drawText(QRectF(left,1,w,ruler-2),Qt::AlignCenter,label);
        }
        p.setPen(QPen(accent,2)); p.drawLine(QPointF(head,0),QPointF(head,height()));
        if (hasFocus()) {
            p.setBrush(Qt::NoBrush); p.setPen(QPen(accent,1,Qt::DotLine));
            p.drawRect(area.adjusted(.5,.5,-.5,-.5));
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button()!=Qt::LeftButton) { QSlider::mousePressEvent(event); return; }
        setFocus(Qt::MouseFocusReason); setSliderDown(true); moveTo(event->position().x()); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!isSliderDown()) { QSlider::mouseMoveEvent(event); return; }
        moveTo(event->position().x()); event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button()!=Qt::LeftButton || !isSliderDown()) { QSlider::mouseReleaseEvent(event); return; }
        moveTo(event->position().x());
        const double target=secondsAt(sliderPosition());
        setSliderDown(false);
        // A fractional smooth pose can share the selected frame's integer slot.
        // It still needs one seek even when QSlider emits no valueChanged.
        if (position!=target) {
            setPosition(target);
            if (seekRequested) seekRequested(target);
        }
        event->accept();
    }
    void keyPressEvent(QKeyEvent* event) override {
        double at=position;
        switch (event->key()) {
        case Qt::Key_Left: case Qt::Key_Down: at=animationStep(position,duration,fps,-1); break;
        case Qt::Key_Right: case Qt::Key_Up: at=animationStep(position,duration,fps,1); break;
        case Qt::Key_Home: at=0; break;
        case Qt::Key_End: at=duration; break;
        case Qt::Key_PageDown: at=nearest(std::max(0.0,position-5)); break;
        case Qt::Key_PageUp: at=nearest(std::min(duration,position+5)); break;
        default: QSlider::keyPressEvent(event); return;
        }
        if (at!=position) {
            setPosition(at);
            if (seekRequested) seekRequested(at);
        }
        event->accept();
    }
private:
    double nearest(double seconds) const {
        if (!std::isfinite(seconds*fps)) return seconds;
        const double lower=std::floor(seconds*fps)/fps;
        const double upper=std::min(duration,lower+1.0/fps);
        return seconds-lower<upper-seconds?lower:upper;
    }
    int indexAt(double seconds) const {
        if (seconds>=duration) return maximum();
        if (std::ceil(duration*fps)<=maximum()) return int(std::floor(animationFrame(seconds,fps)));
        return int(std::clamp(std::round(seconds/duration*maximum()),0.0,double(maximum())));
    }
    void moveTo(double x) {
        const double fraction=std::clamp((x-5)/std::max(1,width()-10),0.0,1.0);
        setSliderPosition(indexAt(nearest(fraction*duration)));
    }
    double duration=0,position=0;
    int fps=12;
};
}
