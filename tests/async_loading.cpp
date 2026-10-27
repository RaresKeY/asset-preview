#include "viewer.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QJsonArray>
#include <QLibrary>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
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
    QCoreApplication::sendPostedEvents(surface.get(),QEvent::MetaCall);
    waitFor([&] { return entered()==2; },false);
    QCoreApplication::sendPostedEvents(qApp,QEvent::MetaCall);
    require(handle.view->loading(),"Old completion finished a newer import");
    require(completions==0,"Obsolete completion reached the service");
    require(handle.view->snapshot().pixelColor(0,0).red()==2,"Fresh frame was not published");
    allow(2);
    waitFor([&] { return completions==1 && !handle.view->loading(); });

    cfg["test_stage"]=3; handle.view->reload(cfg,error);
    QCoreApplication::sendPostedEvents(surface.get(),QEvent::MetaCall);
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
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try { modelPublication(); imageInputDuringDecode(); std::cout<<"Async publication and input checks passed\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
