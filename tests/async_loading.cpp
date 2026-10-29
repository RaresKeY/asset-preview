#include "viewer.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonArray>
#include <QLibrary>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QWheelEvent>
#include <iostream>
#include <stdexcept>

static void require(bool ok,const char* error) { if(!ok)throw std::runtime_error(error); }
template<class Check> static void waitFor(Check check,bool events=true) {
    QElapsedTimer elapsed; elapsed.start();
    while(!check()) {
        require(elapsed.elapsed()<10000,"Timed out waiting for bounded fixture");
        if(events)QApplication::processEvents();
        QThread::msleep(1);
    }
}
static void modelPublication() {
    QLibrary library(QCoreApplication::applicationDirPath()+"/asset-preview-f3d.so");
    library.setLoadHints(QLibrary::PreventUnloadHint);
    require(library.load(),"Cannot load inert worker fixture");
    const auto entered=reinterpret_cast<int (*)()>(library.resolve("fixtureEntered"));
    const auto allow=reinterpret_cast<void (*)(int)>(library.resolve("fixtureAllow"));
    require(entered && allow,"Missing fixture barriers");
    int completions=0,cameraChanges=0,playbackChanges=0;
    QJsonObject cfg{{"kind","model"},{"test_stage",1}}; QString error;
    const auto handle=createModel(cfg,nullptr,[&](bool ok,QString) { require(ok,"Fixture failed loading"); ++completions; },
        [&](const QJsonObject&,bool) { ++cameraChanges; });
    std::unique_ptr<QWidget> surface(handle.surface);
    auto* receiver=dynamic_cast<QObject*>(handle.view);
    const auto unblock=qScopeGuard([&] { allow(99); });
    handle.view->playbackSettingsRequested=[&](QJsonObject) { ++playbackChanges; };
    require(handle.view->reload(cfg,error),"First reload rejected");
    waitFor([&] { return entered()==1; });
    allow(1);
    waitFor([&] { return !handle.view->metrics()["command_pending"].toBool(); },false);
    require(completions==0,"GUI callback ran without event dispatch");
    cfg["test_stage"]=2; handle.view->reload(cfg,error);
    // Run the surface's deferred reload without delivering qApp's older
    // coalesced completion. Barriers make this race deterministic.
    QCoreApplication::sendPostedEvents(receiver,QEvent::MetaCall);
    waitFor([&] { return entered()==2; },false);
    QCoreApplication::sendPostedEvents(qApp,QEvent::MetaCall);
    require(handle.view->loading(),"Old completion finished a newer import");
    require(completions==0,"Obsolete completion reached the service");
    require(handle.view->snapshot().pixelColor(0,0).red()==2,"Fresh frame was not published");
    allow(2);
    waitFor([&] { return completions==1 && !handle.view->loading(); });

    cfg["test_stage"]=3; handle.view->reload(cfg,error);
    QCoreApplication::sendPostedEvents(receiver,QEvent::MetaCall);
    waitFor([&] { return entered()==3; },false);
    cfg["test_stage"]=4; handle.view->reload(cfg,error);
    QCoreApplication::sendPostedEvents(qApp,QEvent::MetaCall);
    require(cameraChanges==0,"Old revision overwrote current camera state");
    require(playbackChanges==0,"Old revision overwrote current playback settings");
    allow(3);
    waitFor([&] { return completions==2 && !handle.view->loading(); });
    require(handle.view->snapshot().pixelColor(0,0).red()==4,"Replacement frame did not win");
}
static void imageInputDuringDecode() {
    QTemporaryDir root; require(root.isValid(),"Cannot create image fixture directory");
    const auto path=root.filePath("input.png");
    QImage image(256,256,QImage::Format_RGBA8888); image.fill(Qt::red);
    require(image.save(path),"Cannot save fixture image");
    ImageView view; bool loaded=false; QString error;
    view.loaded=[&](bool ok,QString) { require(ok,"Fixture image decode failed"); loaded=true; };
    const QJsonObject cfg{{"path",path},{"image_view",QJsonObject{{"zoom",3},{"pan",QJsonArray{.2,.3}}}}};
    require(view.reload(cfg,error),"Fixture image rejected");
    require(view.inspectionState()["zoom"].toDouble()==3,"Saved framing was not restored at admission");
    view.fit(); // Completion is still queued even if the decoder finishes now.
    waitFor([&] { return loaded; });
    require(view.inspectionState()["zoom"].toDouble()==1,"Decode overwrote a new Fit request");
    require(view.inspectionState()["pan"].toArray()==QJsonArray{0,0},"Decode overwrote new pan state");
}
static void modelMotionBacklog() {
    QLibrary library(QCoreApplication::applicationDirPath()+"/asset-preview-f3d.so");
    const auto reads=reinterpret_cast<int (*)()>(library.resolve("fixtureMetricReads"));
    require(reads,"Missing metrics fixture counter");
    const QJsonObject cfg{{"kind","model"},{"test_stage",5}}; QString error;
    bool loaded=false; QJsonObject saved;
    const auto handle=createModel(cfg,nullptr,[&](bool ok,QString) { require(ok,"Motion fixture failed loading"); loaded=true; },
        [&](const QJsonObject& camera,bool persist) { if(persist)saved=camera; });
    std::unique_ptr<QWidget> surface(handle.surface);
    require(handle.view->reload(cfg,error),"Motion fixture rejected");
    waitFor([&] { return loaded && !handle.view->metrics()["command_pending"].toBool(); });
    const auto initialReads=reads();
    auto mouse=[&](QEvent::Type type,QPointF point,Qt::MouseButton button,Qt::MouseButtons buttons,Qt::KeyboardModifiers modifiers=Qt::NoModifier) {
        QMouseEvent event(type,point,point,button,buttons,modifiers);
        QApplication::sendEvent(dynamic_cast<QObject*>(handle.view),&event);
    };
    QPointF point(10,10); QString trace;
    mouse(QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);
    for(int i=0;i<96;++i) {
        if(i==48) { handle.view->fit(); trace+='f'; }
        if(i==24 || i==72) {
            QWheelEvent event(point,point,{},QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
            QApplication::sendEvent(dynamic_cast<QObject*>(handle.view),&event); trace+='w';
        }
        point+=QPointF(1,i%2?-1:1);
        const bool pan=i%3==0;
        mouse(QEvent::MouseMove,point,Qt::NoButton,Qt::LeftButton,pan?Qt::ShiftModifier:Qt::NoModifier);
        trace+=pan?'p':'o';
    }
    mouse(QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton);
    waitFor([&] { return !handle.view->metrics()["command_pending"].toBool() && handle.view->snapshot().pixelColor(0,0).red()==96; });
    require(saved["moves"].toInt()==96 && saved["trace"].toString()==trace,"Motion/release reordered or dropped input across Fit");
    require(saved["x"].toDouble()==96 && saved["y"].toDouble()==0,"Motion deltas were lost");
    const auto metrics=handle.view->metrics();
    std::cout<<"Motion backlog: "<<metrics["renders"].toInt()<<" frames for 96 events\n";
    require(metrics["renders"].toInt()<=8,"Queued mouse motion rendered stale intermediate frames");
    std::cout<<"Motion metadata: "<<reads()-initialReads<<" publications\n";
    require(reads()-initialReads<=16,"Mouse events serialized metadata individually instead of per batch/frame");
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try { modelPublication(); imageInputDuringDecode(); modelMotionBacklog(); std::cout<<"Async publication and input checks passed\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
