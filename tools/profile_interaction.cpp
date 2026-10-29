#include "viewer.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QOpenGLWindow>
#include <QThread>
#include <QTimer>
#include <iostream>
#include <stdexcept>
#include <vector>

// Run beside the selected backend module, under tools/offscreen.sh (or the
// private Wayland harness). This measures Qt input delivery to completed-frame
// repaint admission, not physical display/scanout latency.
class Paints final : public QObject {
public:
    PreviewView* view=nullptr;
    QElapsedTimer clock;
    std::vector<double> paints;
    double settled=-1;
    int finalRenders=-1;
    int lastPaintRenders=-1;
    bool finalInput=false;
    void frame() {
        if(clock.isValid()) {
            paints.push_back(clock.nsecsElapsed()/1e6);
            const auto metrics=view->metrics();
            lastPaintRenders=metrics["renders"].toInt();
            if(finalInput && settled<0 && !metrics["command_pending"].toBool() && !metrics["render_pending"].toBool()) {
                settled=paints.back(); finalRenders=metrics["renders"].toInt();
            }
        }
    }
    bool eventFilter(QObject*,QEvent* event) override {
        if(event->type()==QEvent::Paint)frame();
        return false;
    }
};
int main(int argc,char** argv) {
    auto format=QSurfaceFormat::defaultFormat();
    format.setRenderableType(QSurfaceFormat::OpenGL); format.setVersion(3,3);
    format.setProfile(QSurfaceFormat::CoreProfile); format.setDepthBufferSize(24); format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv);
    try {
        if(argc<2)throw std::runtime_error("Expected model path [burst|stream] [capture.png]");
        const bool burst=argc>2 && QString::fromLocal8Bit(argv[2])=="burst";
        const QJsonObject cfg{{"kind","model"},{"path",QString::fromLocal8Bit(argv[1])},
            {"settings",QJsonObject{{"lock_horizon",true},{"paused",true},{"grid",false},{"orthographic",true}}}};
        QString error; bool loaded=false;
        const auto handle=createModel(cfg,nullptr,[&](bool ok,QString failure) {
            if(!ok)error=failure;
            loaded=true;
        },{});
        std::unique_ptr<QWidget> surface(handle.surface);
        surface->resize(1000,680); surface->show(); handle.view->reload(cfg,error);
        QElapsedTimer deadline; deadline.start();
        auto wait=[&](auto done) {
            while(!done()) {
                if(!error.isEmpty())throw std::runtime_error(error.toStdString());
                if(deadline.elapsed()>60000)throw std::runtime_error("Interaction timed out");
                app.processEvents(); QThread::msleep(1);
            }
        };
        wait([&]{return loaded && !handle.view->metrics()["command_pending"].toBool();});
        for(int i=0;i<100;++i){app.processEvents();QThread::msleep(1);}
        const auto renderer=handle.view->metrics()["renderer"].toString();
        if(renderer.isEmpty() || renderer.contains("llvmpipe",Qt::CaseInsensitive) || renderer=="unknown")
            throw std::runtime_error("Hardware renderer required");
        const int before=handle.view->metrics()["renders"].toInt();
        const auto initial=handle.view->cameraState();
        Paints observer; observer.view=handle.view; surface->installEventFilter(&observer);
        auto* native=dynamic_cast<QObject*>(handle.view);
        if(auto* window=qobject_cast<QOpenGLWindow*>(native))
            QObject::connect(window,&QOpenGLWindow::frameSwapped,&observer,[&observer]{observer.frame();});
        auto mouse=[&](QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons) {
            QMouseEvent event(type,point,surface->mapToGlobal(point),button,buttons,Qt::NoModifier);
            QApplication::sendEvent(native,&event);
        };
        QPointF point(300,300); mouse(QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);
        constexpr int count=96;
        observer.clock.start(); double stopped=0;
        for(int i=0;i<count;++i) {
            point+=QPointF(2,i%2?-.5:.5);
            mouse(QEvent::MouseMove,point,Qt::NoButton,Qt::LeftButton);
            if(!burst) {
                const auto target=(i+1)*8.;
                while(observer.clock.nsecsElapsed()/1e6<target){app.processEvents();QThread::msleep(1);}
            }
        }
        stopped=observer.clock.nsecsElapsed()/1e6;
        mouse(QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton);
        observer.finalInput=true;
        wait([&]{
            const auto metrics=handle.view->metrics();
            if(metrics["command_pending"].toBool() || metrics["render_pending"].toBool())return false;
            if(observer.lastPaintRenders!=metrics["renders"].toInt())return false;
            // A fast producer can display the final move before release; the
            // release only persists its camera and does not need another frame.
            if(observer.settled<0) {
                observer.settled=std::max(stopped,observer.paints.empty()?stopped:observer.paints.back());
                observer.finalRenders=metrics["renders"].toInt();
            }
            return true;
        });
        // Settling must also stay settled, with no recurring static render loop.
        const int renders=observer.finalRenders;
        for(int i=0;i<80;++i){app.processEvents();QThread::msleep(1);}
        if(handle.view->metrics()["renders"].toInt()!=renders)throw std::runtime_error("Static rendering did not settle");
        QJsonArray paints; for(double at:observer.paints)paints.append(at);
        const QJsonValue readbacks=handle.view->metrics()["display_readbacks"];
        const auto frame=handle.view->snapshot().convertToFormat(QImage::Format_RGBA8888);
        const auto beforeThumbnail=handle.view->metrics();
        const auto thumbnail=handle.view->thumbnailSnapshot({256,160});
        if(thumbnail.isNull() || thumbnail.width()>256 || thumbnail.height()>160)
            throw std::runtime_error("Bounded departure thumbnail missing");
        if(handle.view->metrics()["renders"]!=beforeThumbnail["renders"])
            throw std::runtime_error("Departure thumbnail unnecessarily rendered the scene");
        const auto digest=QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(frame.constBits()),frame.sizeInBytes()),QCryptographicHash::Sha256).toHex();
        if(argc>3 && !frame.save(QString::fromLocal8Bit(argv[3])))throw std::runtime_error("Cannot save interaction frame");
        const QJsonObject result{{"renderer",renderer},{"platform",QGuiApplication::platformName()},
            {"scope","Qt mouse delivery to native frameSwapped callback; no physical input or scanout clock"},
            {"mode",burst?"burst":"125Hz stream"},{"events",count},{"input_end_ms",stopped},
            {"settled_ms",observer.settled},{"tail_ms",observer.settled-stopped},
            {"renders",renders-before},{"paint_ms",paints},{"initial_camera",initial},{"final_camera",handle.view->cameraState()},
            {"readbacks_during_input",readbacks},{"frame_transport",handle.view->metrics()["frame_transport"]},
            {"gpu_buffers",handle.view->metrics()["gpu_buffers"]},{"gpu_color_bytes",handle.view->metrics()["gpu_color_bytes"]},
            {"thumbnail_readback_bytes",handle.view->metrics()["display_readback_bytes"].toInteger()-beforeThumbnail["display_readback_bytes"].toInteger()},
            {"final_frame_sha256",QString::fromLatin1(digest)}};
        std::cout<<QJsonDocument(result).toJson(QJsonDocument::Compact).constData()<<'\n';
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
