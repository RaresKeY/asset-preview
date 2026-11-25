#include "viewer.h"
#include "preview_cache.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QLibrary>
#include <QMouseEvent>
#include <QPainter>
#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QWheelEvent>
#include <iostream>
#include <stdexcept>
#include <utility>

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
    require(handle.view->metrics()["loads"].toInt()==2,"Fresh render metadata was not published");
    require(handle.view->snapshot().isNull(),"A pending import attempted a blocking capture");
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
    view.settings({{"settings",QJsonObject{{"nearest",false}}}});
    waitFor([&] { return loaded; });
    require(view.inspectionState()["zoom"].toDouble()==1,"Decode overwrote a new Fit request");
    require(view.inspectionState()["pan"].toArray()==QJsonArray{0,0},"Decode overwrote new pan state");
    require(!view.metrics()["nearest"].toBool() && !view.metrics()["filtering_auto"].toBool(),"Decode overwrote an in-flight manual AA choice");
}
static void imageAutomaticFiltering() {
    QTemporaryDir root; const auto path=root.filePath("filtering.png");
    ImageView view; view.resize(320,320); view.show(); QString error;
    auto load=[&](int width,int height,QJsonObject settings=QJsonObject{}) {
        QImage pixels(width,height,QImage::Format_RGBA8888);
        for(int y=0;y<height;++y)for(int x=0;x<width;++x)
            pixels.setPixelColor(x,y,(x+y)%2?Qt::white:Qt::black);
        require(pixels.save(path),"Cannot save filtering fixture");
        const auto loads=view.metrics()["loads"].toInt();
        require(view.reload({{"path",path},{"settings",settings}},error),"Filtering image rejected");
        waitFor([&] { return !view.loading(); });
        require(view.metrics()["loads"].toInt()==loads+1,"Filtering fixture failed publication");
        QApplication::processEvents();
    };
    load(16,16);
    require(view.metrics()["nearest"].toBool() && view.metrics()["filtering_auto"].toBool(),"Small raster did not default to no AA");
    const auto automatic=view.snapshot();
    // These are opaque black/white source texels; no interpolated gray may
    // appear when magnified with automatic AA off.
    for(int y=0;y<automatic.height();++y)for(int x=0;x<automatic.width();++x) {
        const auto color=automatic.pixelColor(x,y);
        require(color==QColor(Qt::black) || color==QColor(Qt::white),"Automatic no-AA image interpolated source texels");
    }
    view.settings({{"settings",QJsonObject{{"nearest",true}}}}); QApplication::processEvents();
    require(view.snapshot()==automatic,"Automatic low-res filtering differs from explicit no AA");
    view.settings({{"settings",QJsonObject{{"nearest",false}}}}); QApplication::processEvents();
    require(view.snapshot()!=automatic,"Explicit AA failed to smooth the low-res image");
    const auto evidence=qEnvironmentVariable("ASSET_PREVIEW_EVIDENCE_DIR");
    if(!evidence.isEmpty()) {
        const auto backend=view.metrics()["paint_backend"].toString();
        require(automatic.save(evidence+"/"+backend+"-automatic-aa-off.png"),"Cannot save automatic AA evidence");
        require(view.snapshot().save(evidence+"/"+backend+"-manual-aa-on.png"),"Cannot save manual AA evidence");
    }
    view.settings({{"settings",QJsonObject{{"background","dark"},{"nearest",false}}}});
    require(!view.metrics()["nearest"].toBool() && !view.metrics()["filtering_auto"].toBool(),"Display settings lost the explicit AA override");
    view.settings({{"settings",QJsonObject{{"nearest","auto"}}}}); QApplication::processEvents();
    require(view.snapshot()==automatic,"Returning to automatic did not restore crisp pixels");
    load(256,256); require(view.metrics()["nearest"].toBool(),"256px raster should use no AA");
    load(257,257); require(!view.metrics()["nearest"].toBool(),"Larger raster should default to AA");
    load(512,1); require(!view.metrics()["nearest"].toBool(),"Short edge alone classified a large raster as low-res");
    load(257,257,{{"nearest",true}}); require(view.metrics()["nearest"].toBool(),"Large-image no-AA override was ignored");
    load(16,16,{{"nearest",false}}); require(!view.metrics()["nearest"].toBool(),"Low-res AA override was ignored");
    load(16,16,{{"nearest","auto"}}); require(view.metrics()["nearest"].toBool(),"Explicit auto did not use source resolution");
    require(!view.reload({{"path",root.filePath("missing.png")}},error),"Missing filtering source accepted");
    require(view.metrics()["nearest"].toBool(),"Failed reload changed last-good filtering classification");
    const auto vectorPath=root.filePath("small.svg"); QFile svg(vectorPath);
    require(svg.open(QIODevice::WriteOnly),"Cannot write vector filtering fixture");
    svg.write("<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><path d='M0 0L16 16H0Z'/></svg>"); svg.close();
    const auto loads=view.metrics()["loads"].toInt();
    require(view.reload({{"path",vectorPath}},error),"Vector filtering fixture rejected");
    waitFor([&] { return !view.loading(); });
    require(view.metrics()["loads"].toInt()==loads+1 && !view.metrics()["nearest"].toBool(),"Small vector artwork should retain AA");

    class Cache final : public preview::CacheStore {
    public:
        preview::CachedPayload payload;
        preview::CachedPayload take(const QString&,bool) override { return std::exchange(payload,{}); }
        void put(const QString&,preview::CachedPayload) override {}
    } cache;
    QImage retained(16,16,QImage::Format_RGBA8888); retained.fill(Qt::white);
    cache.payload={retained,QSize(4097,4097),{}}; view.cache=&cache;
    require(view.reload({{"path",path}},error),"Cached filtering fixture rejected");
    waitFor([&] { return !view.loading(); });
    require(view.metrics()["cache_hit"].toBool() && !view.metrics()["nearest"].toBool(),"Auto classified a bounded cached copy instead of source dimensions");
    cache.payload={retained,QSize(16,16),{}};
    require(view.reload({{"path",path}},error),"Small cached filtering fixture rejected");
    waitFor([&] { return !view.loading(); });
    require(view.metrics()["nearest"].toBool(),"Small cache hit lost automatic no AA");
    view.cache=nullptr;
}
static void imageCachedPreparation() {
    if(!qEnvironmentVariableIsSet("ASSET_PREVIEW_GPU_TEST"))return;
    class Cache final : public preview::CacheStore {
    public:
        preview::CachedPayload payload;
        int decodes=0;
        preview::CachedPayload take(const QString&,bool) override { return std::exchange(payload,{}); }
        void put(const QString&,preview::CachedPayload next) override { payload=std::move(next); }
        void observe(const QString&,const QString& event,const QString&,qint64,const QString&,qint64,const QJsonObject&) override {
            if(event=="decode")++decodes;
        }
    } cache;
    QTemporaryDir root; const auto path=root.filePath("cached.png"),replacement=root.filePath("replacement.png");
    QImage pixels(2048,2048,QImage::Format_RGBA8888); pixels.fill(QColor(230,50,110,127));
    require(pixels.save(path),"Cannot save cached fixture");
    QImage small(32,32,QImage::Format_RGBA8888); small.fill(Qt::cyan);
    require(small.save(replacement),"Cannot save replacement fixture");
    cache.payload={pixels,pixels.size(),{}};
    ImageView view; view.cache=&cache; QString error;int successes=0,failures=0;
    view.loaded=[&](bool ok,QString) { ok?++successes:++failures; };
    const QJsonObject config{{"path",path},{"image_view",QJsonObject{{"zoom",3},{"pan",QJsonArray{.2,.3}}}}};
    require(view.reload(config,error) && view.loading(),"Large cached GPU pixels were prepared synchronously");
    view.fit();view.panBy({.13,-.21});const auto framing=view.inspectionState();
    waitFor([&] { return !view.loading(); });
    require(successes==1 && failures==0 && cache.decodes==0,"Cache preparation performed a decode or lost completion");
    require(view.pixels()==pixels,"Cache preparation modified source alpha/pixels");
    require(view.inspectionState()==framing,"Cache publication overwrote newer input");
    const auto timing=view.metrics()["load_timing"].toObject();
    require(timing["cache_hit"].toBool() && timing["decode_ms"].toDouble()==0 && timing.contains("prepare_ms"),"Cache timing misreported decode/preparation");

    // Superseding a completed-but-undelivered cached preparation must never
    // publish the old source, framing, or completion into the new request.
    cache.payload={pixels,pixels.size(),{}};
    require(view.reload(config,error),"Cached replacement rejected");
    require(view.reload({{"path",replacement}},error),"Fresh replacement rejected");
    waitFor([&] { return !view.loading(); });
    require(successes==2 && failures==0 && view.pixels().convertToFormat(QImage::Format_RGBA8888)==small,"Superseded cached preparation published stale pixels/completion");
    cache.payload={pixels,pixels.size(),{}};
    require(view.reload(config,error),"Cached freshness check rejected");
    require(small.save(path),"Cannot replace cached source");
    waitFor([&] { return !view.loading(); });
    require(successes==2 && failures==1 && view.pixels().convertToFormat(QImage::Format_RGBA8888)==small,"Changed cached source replaced the last good image");

    // A tiny packed cache payload can still require a large RGBA display copy.
    QImage packed(1024,1024,QImage::Format_Mono);packed.fill(1);
    require(packed.save(path),"Cannot save packed image fixture");
    cache.payload={packed,packed.size(),{}};
    require(view.reload(config,error) && view.loading(),"Packed image bypassed the display-pixel preparation budget");
    waitFor([&] { return !view.loading(); });
    require(successes==3 && failures==1 && view.pixels()==packed,"Packed cached preparation changed source pixels");
}
static void imageDragBackgroundPainting() {
    class Background final : public QWidget {
    public:
        int paints=0;
        void paintEvent(QPaintEvent*) override { ++paints; QPainter(this).fillRect(rect(),Qt::magenta); }
    } parent;
    parent.resize(480,360);
    ImageView view(&parent); view.setGeometry(parent.rect());
    QTemporaryDir root; const auto path=root.filePath("alpha.png");
    QImage pixels(64,64,QImage::Format_RGBA8888); pixels.fill(Qt::transparent);
    { QPainter p(&pixels); p.fillRect(8,8,24,40,QColor(240,60,90,128)); p.fillRect(32,16,24,40,Qt::cyan); }
    require(pixels.save(path),"Cannot save alpha image fixture");
    bool loaded=false; view.loaded=[&](bool ok,QString) { require(ok,"Alpha image decode failed"); loaded=true; };
    QString error; require(view.reload({{"path",path}},error),"Alpha image rejected");
    parent.show(); view.show(); waitFor([&] { return loaded; });
    if(auto* display=view.findChild<QOpenGLWidget*>())display->hide();
    auto flush=[] { QCoreApplication::sendPostedEvents(nullptr,QEvent::UpdateRequest); QApplication::processEvents(); };
    flush(); parent.paints=0;
    view.panBy({.02,.01}); flush();
    require(parent.paints==0,"Image drag repainted the obscured parent background");
    for(const auto& background:{"checker","dark","light"}) for(bool nearest:{false,true}) for(double zoom:{.5,2.5}) {
        view.settings({{"settings",QJsonObject{{"background",background},{"nearest",nearest}}}});
        view.setImageView({{"zoom",zoom},{"pan",QJsonArray{.1,-.15}}});
        const auto opaque=view.snapshot().convertToFormat(QImage::Format_RGBA8888);
        view.setAttribute(Qt::WA_OpaquePaintEvent,false);
        require(opaque==view.snapshot().convertToFormat(QImage::Format_RGBA8888),"Opaque repaint changed alpha, filtering or background pixels");
        view.setAttribute(Qt::WA_OpaquePaintEvent);
    }
    // Negative control: the old transparent-widget path must exercise that work.
    view.setAttribute(Qt::WA_OpaquePaintEvent,false); flush(); parent.paints=0;
    view.panBy({-.02,-.01}); flush();
    require(parent.paints>0,"Parent repaint negative control did not run");
}
static void imageGpuPainting() {
    if(!qEnvironmentVariableIsSet("ASSET_PREVIEW_GPU_TEST"))return;
    QTemporaryDir root; const auto path=root.filePath("gpu-alpha.png");
    QImage pixels(7,5,QImage::Format_RGBA8888);
    for(int y=0;y<pixels.height();++y)for(int x=0;x<pixels.width();++x)
        pixels.setPixelColor(x,y,(x+y)%3==0?QColor(240,40,80,128):(x+y)%3==1?QColor(30,190,230):QColor(Qt::transparent));
    require(pixels.save(path),"Cannot save GPU alpha fixture");
    ImageView view; view.resize(480,360); view.show(); QString error;
    auto* display=view.findChild<QOpenGLWidget*>();
    QApplication::processEvents();
    require(display && !display->isValid(),"Empty image initialized a GPU display");
    const auto invalid=root.filePath("incomplete.png");
    { QFile file(invalid); require(file.open(QIODevice::WriteOnly),"Cannot create incomplete image"); file.write("incomplete image"); }
    require(view.reload({{"path",invalid}},error),"Incomplete image was not queued");
    waitFor([&] { return !view.loading(); });
    require(!display->isValid() && view.metrics()["loads"].toInt()==0,"Failed decode initialized a GPU display");
    require(view.reload({{"path",path}},error),"GPU image rejected");
    waitFor([&] { return !view.loading(); });
    require(display && display->isValid(),"Native image did not create a GPU display");
    display->makeCurrent();
    const auto renderer=QString::fromLatin1(reinterpret_cast<const char*>(display->context()->functions()->glGetString(GL_RENDERER)));
    display->doneCurrent();
    require(!renderer.isEmpty() && !renderer.contains("llvmpipe",Qt::CaseInsensitive),"Image GPU lane used software rendering");
    for(const auto& background:{"checker","dark","light"})for(double zoom:{.53,2.5}) {
        QImage nearest;
        for(bool filteringNearest:{true,false}) {
            view.settings({{"settings",QJsonObject{{"background",background},{"nearest",filteringNearest}}}});
            view.setImageView({{"zoom",zoom},{"pan",QJsonArray{.103,-.157}}});
            QApplication::processEvents();
            const auto gpu=view.snapshot().convertToFormat(QImage::Format_RGBA8888);
            require(!gpu.isNull(),"GPU image capture was empty");
            // Hiding the display exercises the independent raster paint path
            // with the same source, alpha, framing and filtering settings.
            display->hide();
            const auto raster=view.snapshot().convertToFormat(QImage::Format_RGBA8888);
            display->show();
            qint64 errorSum=0;int largeErrors=0;
            for(int y=0;y<gpu.height();++y)for(int x=0;x<gpu.width();++x) {
                const auto a=gpu.pixelColor(x,y),b=raster.pixelColor(x,y);
                for(const auto difference:{std::abs(a.red()-b.red()),std::abs(a.green()-b.green()),std::abs(a.blue()-b.blue())}) {
                    errorSum+=difference;largeErrors+=difference>3;
                }
            }
            if(double(errorSum)/(gpu.width()*gpu.height()*3)>=1.0 || largeErrors>=gpu.width()*gpu.height()/100) {
                std::cerr<<"GPU parity: "<<background<<" zoom="<<zoom<<" nearest="<<filteringNearest<<" mean="<<double(errorSum)/(gpu.width()*gpu.height()*3)<<" large="<<largeErrors<<'\n';
                const auto evidence=qEnvironmentVariable("ASSET_PREVIEW_EVIDENCE_DIR");
                if(!evidence.isEmpty()) { gpu.save(evidence+"/gpu-alpha-failed.png"); raster.save(evidence+"/raster-alpha-reference.png"); }
            }
            require(double(errorSum)/(gpu.width()*gpu.height()*3)<1.0,"GPU image differs materially from raster reference");
            require(largeErrors<gpu.width()*gpu.height()/100,"GPU image changed filtering, alpha or framing");
            if(filteringNearest)nearest=gpu;
            else require(nearest!=gpu,"Filtering toggle did not change GPU image pixels");
        }
    }
    require(view.pixels().convertToFormat(QImage::Format_RGBA8888)==pixels,"GPU rendering changed source pixels");
    auto settle=[] { for(int i=0;i<100;++i){QApplication::processEvents();QThread::msleep(1);} };
    settle(); const auto renders=view.metrics()["renders"].toInt(); settle();
    require(view.metrics()["renders"].toInt()==renders,"Static GPU image kept repainting");
    std::cout<<"Image GPU renderer: "<<renderer.toStdString()<<'\n';
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
    int playbackNotifications=0;
    handle.view->playbackChanged=[&] { ++playbackNotifications; };
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
    require(playbackNotifications==1,"Camera motion refreshed playback controls beyond the explicit Fit command");
}
static void modelPlaybackNotifications() {
    const QJsonObject cfg{{"kind","model"},{"test_stage",6}}; QString error;
    bool loaded=false; int notifications=0;
    const auto handle=createModel(cfg,nullptr,[&](bool ok,QString) { require(ok,"Playback fixture failed loading"); loaded=true; },{});
    std::unique_ptr<QWidget> surface(handle.surface);
    require(handle.view->reload(cfg,error),"Playback fixture rejected");
    waitFor([&] { return loaded && !handle.view->metrics()["command_pending"].toBool(); });
    handle.view->playbackChanged=[&] { ++notifications; };
    handle.view->settings(cfg);
    waitFor([&] { return handle.view->metrics()["renders"].toInt()==20; });
    require(notifications==2,"Frame publication bypassed playback notification cadence or lost a control update");
}
static void modelCallbackRetirement() {
    for(const int notification:{0,1,2}) {
        const QJsonObject cfg{{"kind","model"},{"test_stage",7}}; QString error;
        std::unique_ptr<QWidget> surface; bool retired=false;
        const auto handle=createModel(cfg,nullptr,[&](bool ok,QString) {
            require(ok,"Retirement fixture failed loading");
            if(notification==1) { retired=true; surface.reset(); }
        },{});
        surface.reset(handle.surface);
        if(notification==0)handle.view->framePresented=[&] { retired=true; surface.reset(); };
        if(notification==2)handle.view->playbackChanged=[&] { retired=true; surface.reset(); };
        require(handle.view->reload(cfg,error),"Retirement fixture rejected");
        QCoreApplication::sendPostedEvents(dynamic_cast<QObject*>(handle.view),QEvent::MetaCall);
        waitFor([&] { return !handle.view->metrics()["command_pending"].toBool(); },false);
        // Deliver a frame, completion and playback notice in one mailbox. A
        // consumer may navigate away from any callback and delete the view.
        QCoreApplication::sendPostedEvents(qApp,QEvent::MetaCall);
        require(retired && !surface,"Callback did not retire the native view");
    }
}
int main(int argc,char** argv) {
    if(qEnvironmentVariableIsSet("ASSET_PREVIEW_GPU_TEST")) {
        QSurfaceFormat format;format.setRenderableType(QSurfaceFormat::OpenGL);format.setVersion(3,3);format.setProfile(QSurfaceFormat::CoreProfile);
        format.setDepthBufferSize(24);format.setStencilBufferSize(8);format.setSamples(0);
        QSurfaceFormat::setDefaultFormat(format);
    }
    QApplication app(argc,argv);
    try { modelPublication(); imageInputDuringDecode(); imageAutomaticFiltering(); imageCachedPreparation(); imageDragBackgroundPainting(); imageGpuPainting(); modelMotionBacklog(); modelPlaybackNotifications(); modelCallbackRetirement(); std::cout<<"Async publication, input, automatic image filtering, image repaint and callback retirement checks passed\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
