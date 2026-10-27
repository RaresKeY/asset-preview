#include "fullscreen_notice.h"
#include "video_controls.h"
#include "animation_controls.h"
#include "animation_time.h"
#include "animation_timeline.h"
#include <QJsonArray>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QPainter>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>
#include <cstdlib>

namespace {
class LayoutEvents final : public QObject {
public:
    int count=0;
protected:
    bool eventFilter(QObject*,QEvent* event) override {
        if (event->type()==QEvent::LayoutRequest) ++count;
        return false;
    }
};
void require(bool condition,const QString& message) {
    if (!condition) { QTextStream(stderr)<<message<<'\n'; std::exit(1); }
}
void settle() { QApplication::processEvents(); QApplication::processEvents(); }
void checkBounds(VideoControls& controls,QWidget& window) {
    const auto data=controls.describe(&window);
    QList<QRect> visible;
    for (auto it=data.begin();it!=data.end();++it) {
        const auto item=it.value().toObject();
        if (!item["visible"].toBool()) continue;
        const QRect box(item["x"].toInt(),item["y"].toInt(),item["width"].toInt(),item["height"].toInt());
        require(controls.geometry().contains(box),QString("%1 extends beyond the %2 x %3 playback bar: %4,%5 %6 x %7 (font %8)")
            .arg(it.key()).arg(controls.width()).arg(controls.height()).arg(box.x()).arg(box.y()).arg(box.width()).arg(box.height())
            .arg(controls.font().pointSize()));
        for (const auto& other:visible) require(!other.intersects(box),it.key()+" overlaps another control");
        visible.append(box);
    }
    for (auto* label:controls.findChildren<QLabel*>()) if (label->isVisible()) {
        require(label->width()>=label->sizeHint().width(),"Playback time is horizontally clipped");
        require(label->contentsRect().height()>=label->fontMetrics().height(),"Playback time is vertically clipped");
    }
}
}

int main(int argc,char** argv) {
    require(preview::animationSample(.749,7.235,true,12)==8.0/12,"Stepped sampling moved before its boundary");
    require(preview::animationSample(7.235,7.235,true,12)==7.235,"Stepped EOF discarded the partial final frame");
    require(preview::animationStep(0,7.235,12,-1)==0,"Previous frame escaped clip start");
    require(preview::animationStep(7.2,7.235,12,1)==7.235,"Next frame escaped clip end");
    require(preview::animationStep(7.235,7.235,12,-1)==86.0/12,"Previous frame skipped the last full frame");
    require(preview::animationStep(1.0/12,7.235,12,1)==2.0/12,"Floating-point frame boundary did not advance");
    require(preview::animationStep(1000000003.0/7,2e8,7,-1)==1000000002.0/7,"Long-clip reverse step stalled at a rounded boundary");
    require(preview::animationSample(1000000004.0/11,2e8,true,11)==1000000004.0/11,"Long-clip sampling fell behind a rounded boundary");
    QApplication app(argc,argv);
    {
        preview::AnimationTimeline timeline(nullptr);
        timeline.resize(610,64); timeline.setClip(7.235,12);
        int commits=0;
        QObject::connect(&timeline,&QSlider::valueChanged,[&] { ++commits; });
        QMouseEvent press(QEvent::MouseButtonPress,QPointF(207,32),QPointF(207,32),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&timeline,&press);
        require(timeline.isSliderDown() && commits==0,"Frame scrubbing committed before release");
        QMouseEvent release(QEvent::MouseButtonRelease,QPointF(305,32),QPointF(305,32),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(&timeline,&release);
        require(commits==1 && !timeline.isSliderDown(),"Frame scrubbing did not commit exactly once");
        require(timeline.secondsAt(timeline.value())==43.0/12,"Timeline did not snap to the nearest frame");
        QKeyEvent end(QEvent::KeyPress,Qt::Key_End,Qt::NoModifier);
        QApplication::sendEvent(&timeline,&end);
        require(timeline.secondsAt(timeline.value())==7.235,"Timeline lost the partial terminal pose");
        QKeyEvent back(QEvent::KeyPress,Qt::Key_Left,Qt::NoModifier);
        QApplication::sendEvent(&timeline,&back);
        require(timeline.secondsAt(timeline.value())==86.0/12,"Timeline skipped the frame before the partial endpoint");
    }
    {
        preview::AnimationTimeline timeline(nullptr); timeline.resize(610,64);
        double requested=-1; int commits=0;
        timeline.seekRequested=[&](double seconds) { requested=seconds; ++commits; };
        for (double duration:{14400.,86400.,1e12}) {
            timeline.setClip(duration,120); timeline.setPosition(12345.0/120);
            QKeyEvent forward(QEvent::KeyPress,Qt::Key_Right,Qt::NoModifier);
            QApplication::sendEvent(&timeline,&forward);
            require(requested==12346.0/120,"Long-clip frame step skipped or stalled");
            QKeyEvent back(QEvent::KeyPress,Qt::Key_Left,Qt::NoModifier);
            QApplication::sendEvent(&timeline,&back);
            require(requested==12345.0/120,"Long-clip reverse step skipped or stalled");
        }
        timeline.setClip(2,12); timeline.setPosition(.74);
        QKeyEvent forward(QEvent::KeyPress,Qt::Key_Right,Qt::NoModifier);
        QApplication::sendEvent(&timeline,&forward);
        require(requested==.75,"Frame stepping rounded a fractional smooth pose before advancing");
        timeline.setPosition(.74); commits=0;
        // Current integer frame is 8; clicking that frame must still snap .74s.
        const QPointF point(5+600*(8.0/12)/2,32);
        QMouseEvent press(QEvent::MouseButtonPress,point,point,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease,point,point,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(&timeline,&press); QApplication::sendEvent(&timeline,&release);
        require(commits==1 && requested==8.0/12,"Clicking the current frame failed to snap the smooth pose exactly once");
        timeline.setClip(7.235,12); commits=0;
        const QPointF nearEnd(5+600*7.205/7.235,32);
        QMouseEvent endPress(QEvent::MouseButtonPress,nearEnd,nearEnd,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent endRelease(QEvent::MouseButtonRelease,nearEnd,nearEnd,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(&timeline,&endPress); QApplication::sendEvent(&timeline,&endRelease);
        require(commits==1 && requested==7.235,"Nearest-frame seeking ignored the shortened terminal interval");
    }
    {
        QWidget window; window.resize(900,220);
        AnimationControls controls(&window); controls.resize(900,180); window.show(); controls.show();
        const QJsonObject clips{{"animations",QJsonArray{"Rig motion","Slide"}},{"duration",2},{"position",.5},{"paused",true}};
        controls.setPlayback({{"animation_fps",12}},clips); settle();
        auto* fps=controls.findChild<QSpinBox*>(); auto* editor=fps->findChild<QLineEdit*>();
        fps->setFocus(); settle(); editor->setText("7 fps"); editor->setModified(true);
        controls.setPlayback({{"animation_fps",24}},clips);
        require(editor->text()=="7 fps","External FPS update overwrote unfinished input");
        require(controls.describe(&window)["time"].toObject()["text"]=="Frame 12 / 48","Frame readout used a stale FPS editor value");
        double target=-1; int seeks=0;
        controls.seekRequested=[&](double seconds) { target=seconds; ++seeks; };
        for (auto* button:controls.findChildren<QToolButton*>())
            if (button->accessibleName().startsWith("Next frame")) button->click();
        require(target==13.0/24,"Frame button used a stale FPS editor value");
        auto* timeline=controls.findChild<QSlider*>();
        const QPointF point(timeline->width()/2,32);
        QMouseEvent press(QEvent::MouseButtonPress,point,point,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(timeline,&press); require(timeline->isSliderDown(),"Drag fixture did not begin");
        auto replacement=clips; replacement["animation"]=1;
        const int before=seeks; controls.setPlayback({{"animation_fps",24}},replacement);
        require(!timeline->isSliderDown(),"Clip replacement retained an old drag");
        QMouseEvent release(QEvent::MouseButtonRelease,point,point,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(timeline,&release);
        require(seeks==before,"Clip replacement committed an old drag into the new clip");
        controls.resize(600,180);
        for (double duration:{12.,1e30,12.}) {
            auto state=clips; state["duration"]=duration; state["position"]=duration-1;
            controls.setPlayback({{"animation_fps",120}},state); settle();
            const auto time=controls.describe(&window)["time"].toObject();
            require(time["visible"].toBool()==(duration==12),"Frame readout did not reflow after a duration change");
        }
    }
    const QFont original=app.font();
    for (const int pointSize:{10,18,26}) {
        auto font=original; font.setPointSize(pointSize); QApplication::setFont(font);
        QWidget window; window.resize(900,300);
        VideoControls controls(&window);
        LayoutEvents layouts; controls.installEventFilter(&layouts); window.installEventFilter(&layouts);
        QVBoxLayout row(&window); row.setContentsMargins(0,0,0,0); row.setSizeConstraint(QLayout::SetNoConstraint);
        row.addWidget(&controls,0,Qt::AlignTop);
        window.show(); controls.show();
        for (const bool condensed:{false,true}) {
            controls.setCondensed(condensed);
            for (const int width:{100,140,180,299,300,329,330,419,420,600,900}) {
                window.resize(width,300); settle();
                require(controls.width()==width,"Playback bar did not reach the requested test width");
                // Property updates must reflow even if the card does not resize.
                for (const double duration:{12.,10000.,360000.,315576000000.,12.}) {
                    controls.setPlayback({{"paused",true}},{{"engine",true},{"seekable",true},
                        {"duration",duration},{"position",duration-1}});
                    settle(); checkBounds(controls,window);
                    if (!condensed && width>=600 && duration==12)
                        require(controls.describe(&window)["time"].toObject()["visible"].toBool(),"Short playback time stays hidden after a long duration");
                }
            }
            for (int i=0;i<6;++i) settle();
            const int count=layouts.count;
            for (int i=0;i<16;++i) settle();
            require(layouts.count==count,"Static playback controls keep requesting layout updates");
        }
        controls.setCondensed(false); window.resize(600,300);
        controls.setPlayback({{"paused",true}},{{"engine",true},{"seekable",true},{"duration",12},{"position",3}});
        for (const int nextSize:{10,26,18}) {
            font.setPointSize(nextSize); QApplication::setFont(font);
            for (int i=0;i<6;++i) settle();
            checkBounds(controls,window);
        }
    }
    for (const int pointSize:{10,18,26}) {
        auto font=original; font.setPointSize(pointSize); QApplication::setFont(font);
        QWidget window; window.resize(900,200);
        AnimationControls controls(&window);
        QVBoxLayout row(&window); row.setContentsMargins(0,0,0,0); row.setSizeConstraint(QLayout::SetNoConstraint);
        row.addWidget(&controls,0,Qt::AlignTop); window.show();
        controls.setPlayback({},{{"animations",QJsonArray{"Rig motion","Slide"}},{"duration",2},{"position",.5},{"paused",true}});
        for (bool condensed:{false,true}) {
            controls.setCondensed(condensed);
            for (int width:{100,160,260,300,350,400,900}) {
                window.resize(width,200); for (int i=0;i<6;++i) settle();
                const auto data=controls.describe(&window);
                for (auto it=data.begin();it!=data.end();++it) {
                    const auto item=it.value().toObject();
                    if (!item["visible"].toBool()) continue;
                    require(controls.geometry().contains(QRect(item["x"].toInt(),item["y"].toInt(),item["width"].toInt(),item["height"].toInt())),
                        "Animation control extends beyond a narrow bar: "+it.key());
                }
                auto* clip=controls.findChild<QComboBox*>();
                require(clip->height()>=clip->sizeHint().height(),"Animation clip text is vertically clipped at font "+QString::number(pointSize));
                auto* speed=controls.findChild<QDoubleSpinBox*>();
                if (speed->isVisible()) {
                    auto* editor=speed->findChild<QLineEdit*>();
                    require(editor->contentsRect().width()>=editor->fontMetrics().horizontalAdvance(editor->displayText()),
                        "Animation speed text is horizontally clipped at font "+QString::number(pointSize));
                }
                if (condensed && pointSize==10) require(controls.height()<=46,"Condensed animation bar overlaps the card header");
            }
        }
        controls.setCondensed(false); window.resize(900,200); settle();
        auto* speed=controls.findChild<QDoubleSpinBox*>();
        speed->setFocus(); settle(); require(speed->hasFocus(),"Animation speed did not receive test focus");
        controls.setPlayback({{"animation_speed",2}},{{"animations",QJsonArray{"Rig motion"}},{"duration",2},{"paused",true}});
        require(speed->value()==2,"Focused animation speed ignores external setting changes");
        auto* editor=speed->findChild<QLineEdit*>();
        editor->setText("3.00×"); editor->setModified(true);
        controls.setPlayback({{"animation_speed",.5}},{{"animations",QJsonArray{"Rig motion"}},{"duration",2},{"paused",false}});
        require(editor->text()=="3.00×","Animation notifications overwrite unfinished speed edits");
        QKeyEvent commit(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);
        QApplication::sendEvent(speed,&commit);
        require(speed->value()==3,"Committed animation speed edit was lost");
        controls.focusPlayback(); settle();
        require(QApplication::focusWidget()->accessibleName()=="Play animation" ||
                QApplication::focusWidget()->accessibleName()=="Pause animation","Forward animation focus misses Play");
        QKeyEvent tab(QEvent::KeyPress,Qt::Key_Tab,Qt::NoModifier);
        QApplication::sendEvent(QApplication::focusWidget(),&tab); settle();
        require(QApplication::focusWidget()->accessibleName().startsWith("Restart animation"),"Animation tab order skips Restart");
        controls.focusPlayback(true); settle();
        require(QApplication::focusWidget()->accessibleName()=="Animation timeline","Backward animation focus misses the timeline");
        for (const int nextSize:{26,10,18}) {
            font.setPointSize(nextSize); QApplication::setFont(font); for (int i=0;i<6;++i) settle();
            auto* clip=controls.findChild<QComboBox*>();
            require(clip->height()>=clip->sizeHint().height(),"Animation clip clips text after a desktop font change");
            require(editor->contentsRect().width()>=editor->fontMetrics().horizontalAdvance(editor->displayText()),
                "Animation speed clips text after a desktop font change");
        }
    }
    QApplication::setFont(original);
    {
        QWidget window; window.resize(900,180);
        VideoControls controls(&window,true);
        QVBoxLayout row(&window); row.setContentsMargins(0,0,0,0); row.setSizeConstraint(QLayout::SetNoConstraint);
        row.addWidget(&controls,0,Qt::AlignTop); window.show();
        auto waveform=std::make_shared<preview::AudioWaveform>();
        waveform->duration=12; waveform->channels=2; waveform->peak=.5;
        for (auto& bin:waveform->bins) bin={-.3f,.5f,-.5f,.2f};
        controls.setWaveform(waveform);
        controls.setPlayback({{"paused",true}},{{"engine",true},{"seekable",true},{"duration",12},{"position",3}});
        for (const bool condensed:{false,true}) {
            controls.setCondensed(condensed);
            for (const int width:{100,140,180,260,400,900}) {
                window.resize(width,180); settle();
                const auto data=controls.describe(&window);
                require(data["timeline"].toObject()["waveform"].toBool(),"Audio waveform is lost during resize");
                require(data["timeline"].toObject()["height"].toInt()==(condensed?28:64),"Audio waveform has the wrong height");
                for (auto it=data.begin();it!=data.end();++it) {
                    const auto item=it.value().toObject();
                    if (!item["visible"].toBool()) continue;
                    require(controls.geometry().contains(QRect(item["x"].toInt(),item["y"].toInt(),item["width"].toInt(),item["height"].toInt())),
                            "Waveform audio controls extend beyond a narrow bar: "+it.key());
                }
            }
        }
        controls.setWaveform({});
        require(!controls.describe(&window)["timeline"].toObject()["waveform"].toBool(),"Audio waveform is retained after unload");
    }
    QWidget canvas; canvas.resize(900,500); canvas.show();
    FullscreenNotice notice(&canvas); notice.announce(); settle();
    const QColor background(173,61,117);
    QImage capture(canvas.size(),QImage::Format_ARGB32_Premultiplied); capture.fill(background);
    { QPainter painter(&capture); notice.compose(painter,&canvas); }
    bool painted=false,checkedCorner=false;
    for (int y=0;y<notice.height();++y) for (int x=0;x<notice.width();++x) {
        const QPoint local(x,y),global=local+notice.pos();
        if (!notice.mask().contains(local)) {
            checkedCorner=true;
            require(capture.pixelColor(global)==background,"Fullscreen capture paints outside the rounded notice");
        } else if (capture.pixelColor(global)!=background) painted=true;
    }
    require(checkedCorner && painted,"Fullscreen notice capture check did not cover its mask");
    QTextStream(stdout)<<"Video layout and fullscreen capture checks passed\n";
}
