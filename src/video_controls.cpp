#include "video_controls.h"
#include "native_overlay.h"
#include "seek_slider.h"
#include <QEvent>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolButton>
#include <algorithm>
#include <cmath>

namespace {
constexpr int TimelineSteps=1000000;
QIcon mediaIcon(QWidget* widget,const QString& name,QStyle::StandardPixmap fallback) {
    const auto source=QIcon::fromTheme(name,widget->style()->standardIcon(fallback));
    QIcon result;
    // Desktop icons can be dark even though the preview overlays are dark.
    // Retain the native glyph and use the overlay's foreground consistently.
    for (const auto mode:{QIcon::Normal,QIcon::Disabled}) {
        auto pixmap=source.pixmap(QSize(16,16),widget->devicePixelRatioF());
        QPainter painter(&pixmap); painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(),mode==QIcon::Disabled?QColor("#63707d"):QColor("#e8edf3")); painter.end();
        result.addPixmap(pixmap,mode);
    }
    return result;
}
QString timestamp(double seconds) {
    const qint64 whole=std::isfinite(seconds)?qint64(std::clamp(seconds,0.0,315576000000.0)):0;
    const auto tail=QString("%1:%2").arg((whole/60)%60).arg(whole%60,2,10,QChar('0'));
    return whole>=3600?QString("%1:%2").arg(whole/3600).arg(tail.rightJustified(5,'0')):tail;
}
QRegion pillRegion(const QRect& box) {
    QRegion region;
    const double radius=box.height()/2.0;
    for (int y=0;y<box.height();++y) {
        const double dy=y+.5-radius;
        const int inset=std::max(0,int(std::ceil(radius-std::sqrt(std::max(0.0,radius*radius-dy*dy))-.5)));
        region+=QRect(box.x()+inset,box.y()+y,box.width()-2*inset,1);
    }
    return region;
}

}

VideoControls::VideoControls(QWidget* parent, bool isAudio) : QFrame(parent), audio(isAudio), media(isAudio?"audio":"video") {
    setObjectName(audio?"viewOverlay":"videoPlayback"); setNativeOverlay(this);
    if (!audio) setStyleSheet(
        "QFrame#videoPlayback {background:#20262e;border:none;}"
        "QLabel {color:#e8edf3;background:#20262e;border:none;}"
        "QToolButton {color:#e8edf3;background:#20262e;border:1px solid transparent;border-radius:20px;padding:0;}"
        "QToolButton:hover {background:#394453;} QToolButton:pressed {background:#485566;}"
        "QToolButton:focus {border-color:#82d9c8;}"
        "QToolButton:checked {background:#35454b;border-color:#658c88;}"
        "QToolButton:disabled {background:#20262e;}");
    setAccessibleName(audio?"Audio playback controls":"Video playback controls");
    controls=new QGridLayout(this);
    controls->setContentsMargins(audio?6:0,audio?2:0,audio?6:0,audio?2:0); controls->setSpacing(audio?3:4);
    auto button=[this](const QString& theme,QStyle::StandardPixmap fallback,const QString& name) {
        auto* result=new QToolButton(this); result->setFixedSize(audio?QSize(32,28):QSize(40,40)); result->setIconSize({16,16});
        result->setIcon(mediaIcon(this,theme,fallback));
        result->setAccessibleName(name); result->setToolTip(name); result->setFocusPolicy(Qt::StrongFocus);
        return result;
    };
    play=button("media-playback-start",QStyle::SP_MediaPlay,"Play "+media);
    restart=button("media-skip-backward",QStyle::SP_MediaSkipBackward,"Restart "+media+" (Home)");
    mute=button("audio-volume-muted",QStyle::SP_MediaVolumeMuted,"Unmute "+media+" (M)"); mute->setCheckable(true);
    loop=button("media-playlist-repeat",QStyle::SP_BrowserReload,"Loop "+media); loop->setCheckable(true);
    if (!audio) {
        fullscreen=button("view-fullscreen",QStyle::SP_TitleBarMaxButton,"Enter full screen (F11 / double click)");
        connect(fullscreen,&QToolButton::clicked,this,[this] { if (fullscreenRequested) fullscreenRequested(); });
    }
    timeline=new preview::SeekSlider(this,!audio); timeline->setRange(0,TimelineSteps); timeline->setFixedHeight(audio?64:14);
    timeline->setMinimumWidth(24); timeline->setFocusPolicy(Qt::StrongFocus);
    timeline->setAccessibleName(audio?"Audio timeline":"Video timeline");
    timeline->setToolTip("Click or drag to seek · Shift+Left / Right: 5 seconds · Home: restart");
    const QString sliderStyle=
        "QSlider::groove:horizontal {height:4px;background:#414b57;border-radius:2px;}"
        "QSlider::sub-page:horizontal {background:#82d9c8;border-radius:2px;}"
        "QSlider::handle:horizontal {width:10px;margin:-4px 0;background:#e8edf3;border:1px solid #e8edf3;border-radius:5px;}"
        "QSlider::handle:horizontal:hover,QSlider::handle:horizontal:focus {background:#82d9c8;border-color:#82d9c8;}"
        "QSlider::sub-page:horizontal:disabled,QSlider::handle:horizontal:disabled {background:#63707d;border-color:#63707d;}";
    timeline->setStyleSheet(sliderStyle+(audio?QString():QString(
        "QSlider {background:transparent;border:none;}"
        "QSlider::groove:horizontal {height:3px;}"
        "QSlider::handle:horizontal {width:9px;margin:-3px 0;background:#82d9c8;border-color:#82d9c8;}"
        "QSlider::handle:horizontal:hover,QSlider::handle:horizontal:focus {background:#e8edf3;border-color:#e8edf3;}")));
    clock=new QLabel(this); clock->setTextFormat(Qt::PlainText); clock->setMinimumWidth(0);
    clock->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    if (!audio) { clock->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Preferred); clock->setContentsMargins(12,0,12,0); clock->setFixedHeight(40); }
    clock->setAccessibleName("Playback time");
    connect(play,&QToolButton::clicked,this,[this] {
        if (settingsRequested) settingsRequested({{"paused",!settings["paused"].toBool(audio)}});
    });
    connect(restart,&QToolButton::clicked,this,[this] { if (seekRequested) seekRequested(0); });
    connect(mute,&QToolButton::clicked,this,[this] {
        if (settingsRequested) settingsRequested({{"muted",!settings["muted"].toBool(!audio)}});
    });
    connect(loop,&QToolButton::clicked,this,[this] {
        if (settingsRequested) settingsRequested({{"loop",!settings["loop"].toBool(true)}});
    });
    connect(timeline,&QSlider::sliderMoved,this,[this] { updateTime(); });
    connect(timeline,&QSlider::valueChanged,this,[this](int value) {
        position=duration*value/TimelineSteps;
        updateTime();
        if (duration>0 && seekRequested) seekRequested(position);
    });
    if (audio) {
        controls->addWidget(timeline,0,0,1,5);
        controls->addWidget(play,1,0); controls->addWidget(restart,1,1);
        controls->addWidget(clock,1,2); controls->addWidget(mute,1,3); controls->addWidget(loop,1,4);
        controls->setColumnStretch(2,1);
    } else {
        controls->addWidget(timeline,0,0,1,7);
        controls->addWidget(play,1,0); controls->addWidget(mute,1,1); controls->addWidget(clock,1,2);
        controls->setColumnStretch(3,1);
        controls->addWidget(restart,1,4); controls->addWidget(loop,1,5); controls->addWidget(fullscreen,1,6);
    }
    if (!audio) {
        QWidget::setTabOrder(timeline,play); QWidget::setTabOrder(play,mute);
        QWidget::setTabOrder(mute,restart); QWidget::setTabOrder(restart,loop);
        QWidget::setTabOrder(loop,fullscreen);
    }
    setPlayback({},{}); place();
}

void VideoControls::setPlayback(const QJsonObject& next,const QJsonObject& metrics) {
    settings=next;
    duration=metrics["duration"].toDouble(); position=metrics["position"].toDouble();
    if (!std::isfinite(duration) || duration<0) duration=0;
    if (!std::isfinite(position) || position<0) position=0;
    const bool ready=metrics["engine"].toBool(), seekable=ready && metrics["seekable"].toBool() && duration>0;
    const bool paused=settings["paused"].toBool(audio), muted=settings["muted"].toBool(!audio);
    const QString playName=paused?"Play "+media:"Pause "+media, muteName=muted?"Unmute "+media:"Mute "+media;
    if (play->accessibleName()!=playName) {
        play->setIcon(mediaIcon(this,paused?"media-playback-start":"media-playback-pause",paused?QStyle::SP_MediaPlay:QStyle::SP_MediaPause));
        play->setAccessibleName(playName); play->setToolTip(playName+" (Space)");
    }
    if (mute->accessibleName()!=muteName) {
        mute->setIcon(mediaIcon(this,muted?"audio-volume-muted":"audio-volume-high",muted?QStyle::SP_MediaVolumeMuted:QStyle::SP_MediaVolume));
        mute->setAccessibleName(muteName); mute->setToolTip(muteName+" (M)");
    }
    mute->setChecked(muted); loop->setChecked(settings["loop"].toBool(true));
    play->setEnabled(ready); mute->setEnabled(ready); loop->setEnabled(ready);
    restart->setEnabled(seekable);
    const QSignalBlocker blocker(timeline);
    timeline->setEnabled(seekable);
    if (!seekable) timeline->setSliderDown(false);
    if (!timeline->isSliderDown()) timeline->setValue(duration>0?qRound(std::clamp(position/duration,0.0,1.0)*TimelineSteps):0);
    timeline->setSingleStep(duration>0?std::clamp(qRound(TimelineSteps/std::max(1.0,duration)),1,TimelineSteps):1);
    timeline->setPageStep(std::min(TimelineSteps,timeline->singleStep()*10));
    const auto previousTime=clock->text();
    updateTime();
    // Hidden labels do not notify their parent's layout when text changes.
    // Reconsider the pill when a new duration fits, without waiting for resize.
    if (!audio && clock->text()!=previousTime) place();
}

void VideoControls::setCondensed(bool value) {
    if (condensed==value) return;
    condensed=value;
    if (!audio) {
        const int extent=condensed?28:40;
        for (auto* widget:{play,restart,mute,loop,fullscreen}) {
            widget->setFixedSize(extent,extent);
            widget->setStyleSheet(QString("QToolButton {border-radius:%1px;}").arg(extent/2));
        }
        timeline->setFixedHeight(condensed?10:14); controls->setSpacing(condensed?2:4);
        place(); updateGeometry(); return;
    }
    for (auto* widget:{static_cast<QWidget*>(timeline),static_cast<QWidget*>(play),static_cast<QWidget*>(restart),
                      static_cast<QWidget*>(clock),static_cast<QWidget*>(mute),static_cast<QWidget*>(loop),
                      static_cast<QWidget*>(fullscreen)}) if (widget) controls->removeWidget(widget);
    controls->setColumnStretch(2,condensed?0:1); controls->setColumnStretch(1,condensed?1:0);
    timeline->setFixedHeight(condensed?28:64);
    if (condensed) {
        controls->addWidget(play,0,0); controls->addWidget(timeline,0,1,1,3); controls->addWidget(mute,0,4);
    } else {
        controls->addWidget(timeline,0,0,1,audio?5:6);
        controls->addWidget(play,1,0); controls->addWidget(restart,1,1);
        controls->addWidget(clock,1,2); controls->addWidget(mute,1,3); controls->addWidget(loop,1,4);
    }
    if (fullscreen) controls->addWidget(fullscreen,condensed?0:1,5);
    place();
}

void VideoControls::setWaveform(std::shared_ptr<const preview::AudioWaveform> data) {
    if (!audio) return;
    static_cast<preview::SeekSlider*>(timeline)->setWaveform(std::move(data));
    timeline->setFixedHeight(condensed?28:64);
}

void VideoControls::setFullscreen(bool value) {
    if (!fullscreen) return;
    const QString name=value?"Exit full screen (Esc / F11)":"Enter full screen (F11 / double click)";
    if (fullscreen->accessibleName()==name) return;
    fullscreen->setAccessibleName(name); fullscreen->setToolTip(name);
    fullscreen->setIcon(mediaIcon(this,value?"view-restore":"view-fullscreen",
        value?QStyle::SP_TitleBarNormalButton:QStyle::SP_TitleBarMaxButton));
}

void VideoControls::focusPlayback(bool backwards) {
    QList<QWidget*> widgets=audio?QList<QWidget*>{timeline,play,restart,mute,loop}:QList<QWidget*>{timeline,play,mute,restart,loop};
    if (fullscreen) widgets.append(fullscreen);
    if (backwards) std::reverse(widgets.begin(),widgets.end());
    for (auto* widget:widgets) if (widget->isVisible() && widget->isEnabled()) {
        widget->setFocus(backwards?Qt::BacktabFocusReason:Qt::TabFocusReason); return;
    }
}

void VideoControls::place() {
    if (!audio) {
        clock->ensurePolished();
        clock->setFixedHeight(std::max(40,clock->fontMetrics().height()));
        const int rowHeight=condensed?28:clock->height();
        if (controls->rowMinimumHeight(1)!=rowHeight) controls->setRowMinimumHeight(1,rowHeight);
        restart->setVisible(!condensed && width()>=420);
        loop->setVisible(!condensed && width()>=300);
        mute->setVisible(width()>=(condensed?130:180));
        fullscreen->setVisible(width()>=(condensed?100:140));
        updateTime();
        int buttons=0,buttonWidth=0;
        for (auto* button:{play,mute,restart,loop,fullscreen}) if (!button->isHidden()) {
            ++buttons; buttonWidth+=button->width();
        }
        // Include both sides of the flexible gap. A fixed breakpoint alone
        // clips long timestamps or text enlarged by the desktop font setting.
        const int required=buttonWidth+clock->sizeHint().width()+(buttons+1)*controls->horizontalSpacing();
        clock->setVisible(!condensed && width()>=std::max(330,required));
        controls->activate(); shape(); return;
    }
    restart->setVisible(!condensed && width()>=350);
    loop->setVisible(!condensed && width()>=260);
    mute->setVisible(!condensed || width()>=150);
    if (fullscreen) fullscreen->setVisible(width()>=150);
    clock->setVisible(!condensed); updateTime();
}
void VideoControls::shape() {
    if (audio) return;
    // Rasterize each capsule at pixel centers. Flattened painter paths reduce
    // these small curves to visibly angular polygons on a native child window.
    // Keep the timeline's full-height hit target, with matching rounded ends.
    QRegion region=pillRegion(timeline->geometry());
    for (auto* widget:{static_cast<QWidget*>(play),static_cast<QWidget*>(mute),static_cast<QWidget*>(clock),
                      static_cast<QWidget*>(restart),static_cast<QWidget*>(loop),static_cast<QWidget*>(fullscreen)}) {
        if (!widget->isHidden()) {
            region+=pillRegion(widget->geometry());
        }
    }
    if (mask()!=region) setMask(region);
}
bool VideoControls::event(QEvent* event) {
    const bool result=QFrame::event(event);
    if (!audio && clock && (event->type()==QEvent::LayoutRequest || event->type()==QEvent::Show || event->type()==QEvent::FontChange)) {
        place();
    }
    return result;
}
void VideoControls::resizeEvent(QResizeEvent* event) { QFrame::resizeEvent(event); place(); }
void VideoControls::updateTime() {
    const double shown=timeline->isSliderDown()?duration*timeline->sliderPosition()/TimelineSteps:position;
    const auto text=timestamp(shown)+" / "+(duration>0?timestamp(duration):QString("--:--"));
    clock->setText(width()>=200?text:timestamp(shown)); clock->setToolTip(text);
    timeline->setToolTip(text+"\nClick or drag to seek · Shift+Left / Right: 5 seconds · Home: restart");
    timeline->setAccessibleDescription(text);
}

QJsonObject VideoControls::describe(QWidget* window) const {
    QJsonObject result;
    for (const auto& [name,widget]:{std::pair{"play",static_cast<QWidget*>(play)},{"restart",static_cast<QWidget*>(restart)},
            {"mute",static_cast<QWidget*>(mute)},{"loop",static_cast<QWidget*>(loop)},
            {"fullscreen",static_cast<QWidget*>(fullscreen)},
            {"timeline",static_cast<QWidget*>(timeline)},{"time",static_cast<QWidget*>(clock)}}) {
        if (!widget) continue;
        const auto p=widget->mapTo(window,QPoint());
        QJsonObject item{{"x",p.x()},{"y",p.y()},{"width",widget->width()},{"height",widget->height()},
            {"visible",widget->isVisible()},{"enabled",widget->isEnabled()},{"focused",widget->hasFocus()}};
        if (auto* button=qobject_cast<QToolButton*>(widget)) { item["checked"]=button->isChecked(); item["name"]=button->accessibleName(); }
        if (widget==timeline) {
            if (audio) item["waveform"]=static_cast<preview::SeekSlider*>(timeline)->hasWaveform();
            item["position"]=duration*timeline->sliderPosition()/TimelineSteps;
            item["duration"]=duration; item["dragging"]=timeline->isSliderDown();
        }
        if (widget==clock) item["text"]=clock->text();
        result[name]=item;
    }
    return result;
}
