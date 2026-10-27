#pragma once
#include "audio_waveform.h"
#include <QMouseEvent>
#include <QPainter>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <algorithm>

namespace preview {
// Native slider geometry, with absolute click/drag seeking rather than page jumps.
// Commit once on release; player notifications must not move the held thumb.
class SeekSlider final : public QSlider {
public:
    explicit SeekSlider(QWidget* parent,bool video=false) : QSlider(Qt::Horizontal,parent), video(video) { setTracking(false); }
    void setWaveform(std::shared_ptr<const AudioWaveform> value) {
        if (waveform==value) return;
        waveform=std::move(value); update();
    }
    bool hasWaveform() const { return bool(waveform); }
protected:
    void paintEvent(QPaintEvent* event) override {
        if (waveform) { paintWaveform(); return; }
        if (!video) { QSlider::paintEvent(event); return; }
        QStyleOptionSlider option; initStyleOption(&option);
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(Qt::NoPen);
        const double span=std::max(1,width()-10), middle=height()/2.0;
        const int offset=QStyle::sliderPositionFromValue(minimum(),maximum(),sliderPosition(),int(span),option.upsideDown);
        painter.setBrush(QColor("#414b57")); painter.drawRoundedRect(QRectF(5,middle-1.5,span,3),1.5,1.5);
        painter.setBrush(QColor(isEnabled()?"#82d9c8":"#63707d"));
        painter.drawRoundedRect(option.upsideDown?QRectF(5+offset,middle-1.5,span-offset,3):QRectF(5,middle-1.5,offset,3),1.5,1.5);
        if (hasFocus() && isEnabled()) painter.setBrush(QColor("#e8edf3"));
        painter.drawEllipse(QPointF(5+offset,middle),4.5,4.5);
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush); painter.setPen(QPen(QColor("#82d9c8"),1,Qt::DotLine));
            painter.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),3,3);
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button()!=Qt::LeftButton) { QSlider::mousePressEvent(event); return; }
        setFocus(Qt::MouseFocusReason); setSliderDown(true); moveTo(event->position()); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!isSliderDown()) { QSlider::mouseMoveEvent(event); return; }
        moveTo(event->position()); event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button()!=Qt::LeftButton || !isSliderDown()) { QSlider::mouseReleaseEvent(event); return; }
        moveTo(event->position()); setSliderDown(false); event->accept();
    }
private:
    void paintWaveform() {
        QStyleOptionSlider option; initStyleOption(&option);
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area=QRectF(rect()).adjusted(5,4,-5,-4);
        painter.setPen(Qt::NoPen); painter.setBrush(QColor("#11171d"));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0,1,0,-1),4,4);
        const double fraction=maximum()>minimum()?double(sliderPosition()-minimum())/(maximum()-minimum()):0;
        const double playhead=area.left()+(option.upsideDown?1-fraction:fraction)*area.width();
        const double scale=area.height()*.22/std::max(.05f,waveform->peak);
        const int columns=std::max(1,int(area.width()));
        painter.setRenderHint(QPainter::Antialiasing,false);
        for (int ch=0;ch<2;++ch) {
            const double middle=area.top()+area.height()*(ch==0?.25:.75);
            painter.setPen(QPen(QColor("#303e48"),1)); painter.drawLine(QPointF(area.left(),middle),QPointF(area.right(),middle));
            for (int column=0;column<columns;++column) {
                const int from=column*AudioWaveform::binCount/columns;
                const int to=std::min(AudioWaveform::binCount,std::max(from+1,(column+1)*AudioWaveform::binCount/columns));
                float low=0, high=0;
                for (int i=from;i<to;++i) {
                    low=std::min(low,waveform->bins[i][ch*2]); high=std::max(high,waveform->bins[i][ch*2+1]);
                }
                const double x=area.left()+(option.upsideDown?columns-1-column:column);
                const bool played=option.upsideDown?x>=playhead:x<=playhead;
                QColor color(ch==0?"#64d8f1":"#91d68b"); color.setAlphaF(isEnabled()?(played?1:.65):.35);
                painter.setPen(QPen(color,1));
                painter.drawLine(QPointF(x,middle-high*scale),QPointF(x,middle-low*scale));
            }
        }
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(0,0,0,90),5)); painter.drawLine(QPointF(playhead,3),QPointF(playhead,height()-3));
        painter.setPen(QPen(QColor("#f3f8fc"),1.5)); painter.drawLine(QPointF(playhead,3),QPointF(playhead,height()-3));
        painter.setPen(Qt::NoPen); painter.setBrush(QColor("#f3f8fc"));
        painter.drawRoundedRect(QRectF(playhead-3,2,6,3),1,1);
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush); painter.setPen(QPen(QColor("#82d9c8"),1,Qt::DotLine));
            painter.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),4,4);
        }
    }
    void moveTo(const QPointF& position) {
        QStyleOptionSlider option; initStyleOption(&option);
        if (video || waveform) {
            setSliderPosition(QStyle::sliderValueFromPosition(minimum(),maximum(),qRound(position.x())-5,std::max(1,width()-10),option.upsideDown));
            return;
        }
        const auto groove=style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderGroove,this);
        const auto handle=style()->subControlRect(QStyle::CC_Slider,&option,QStyle::SC_SliderHandle,this);
        const int span=std::max(1,groove.width()-handle.width());
        setSliderPosition(QStyle::sliderValueFromPosition(minimum(),maximum(),
            qRound(position.x())-groove.x()-handle.width()/2,span,option.upsideDown));
    }
    bool video=false;
    std::shared_ptr<const AudioWaveform> waveform;
};
}
