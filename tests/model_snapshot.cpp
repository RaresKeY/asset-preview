#include "model_worker.h"
#include "backend_loader.h"
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

struct Frames { int* count; void operator()() const { ++*count; } };
static void require(bool ok,const char* error) { if(!ok)throw std::runtime_error(error); }
using Factory=ModelWorker* (*)(QJsonObject);
static bool sameCamera(const QJsonObject& a,const QJsonObject& b) {
    for(const auto* key:{"position","focal","up"}) {
        const auto x=a[key].toArray(),y=b[key].toArray();
        if(x.size()!=3 || y.size()!=3)return false;
        for(int i=0;i<3;++i)if(std::abs(x[i].toDouble()-y[i].toDouble())>1e-10)return false;
    }
    return std::abs(a["angle"].toDouble()-b["angle"].toDouble())<1e-10 &&
        std::abs(a["parallel_span"].toDouble()-b["parallel_span"].toDouble())<1e-10;
}
static void cameraMotion(Factory factory,QJsonObject cfg) {
    for(const auto* axis:{"y","z"})for(bool orthographic:{false,true}) {
        cfg["settings"]=QJsonObject{{"up_axis",axis},{"orthographic",orthographic},{"lock_horizon",true},{"grid",false}};
        auto run=[&](bool eager,bool perturb=false) {
            std::unique_ptr<ModelWorker> worker(factory(cfg)); QString error;
            worker->viewport({128,128},1); worker->exposed(true);
            require(worker->reload(cfg,error),qPrintable(error));
            std::vector<QJsonObject> checkpoints;
            auto drag=[&](QPointF delta,bool pan=false) { worker->drag(delta,pan); if(eager)worker->cameraState(); };
            for(int i=0;i<96;++i) {
                // Crossing a pole and reversing must keep the per-event clamp.
                drag({2,i==8?1000.0:(i==9?-2.0:0.5)});
                if(i==16) {
                    for(int j=0;j<64;++j)drag({j%3?-3.5:4.25,j%2?-.75:1.25},true);
                    worker->scroll(120);
                }
                if(i==32)worker->fit();
                if(i==48)worker->cameraView("front");
                if(i==64)worker->resetView();
                if(i==72) {
                    const auto saved=worker->cameraState();
                    drag({4,3}); worker->setCameraState(saved);
                }
                if(i==76) {
                    for(int j=0;j<16;++j)drag({.5,-.25},true);
                    worker->viewport({160,144},1);
                    for(int j=0;j<16;++j)drag({-.75,.5},true);
                    worker->viewport({128,128},1);
                }
                if(i==80) {
                    auto next=cfg; auto settings=next["settings"].toObject(); settings["lock_horizon"]=false;
                    next["settings"]=settings; worker->settings(next);
                }
                if(i%16==15)checkpoints.push_back(worker->cameraState());
            }
            for(int j=0;j<64;++j)drag({j%2?-2.25:1.5,j%3?.125:-.5},true);
            if(perturb)drag({20,0});
            checkpoints.push_back(worker->cameraState());
            return std::pair{checkpoints,worker->snapshot()};
        };
        const auto reference=run(true),batched=run(false);
        require(reference.first.size()==batched.first.size(),"Camera checkpoint count differs");
        for(size_t i=0;i<reference.first.size();++i)
            require(sameCamera(reference.first[i],batched.first[i]),"Batched camera differs across an input barrier");
        require(!reference.second.isNull() && reference.second==batched.second,"Batched camera changed captured pixels");
        require(!sameCamera(reference.first.back(),run(false,true).first.back()),"Camera parity gate missed wrong input");
    }
    std::cout<<"Eager/batched orbit, pole clamp, pan bursts, resize, zoom, Fit, view, reset, restore and settings parity passed\n";
}
static void nativePresentation(Factory factory,const QJsonObject& cfg) {
    QOpenGLContext context;
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL); format.setVersion(3,3);
    format.setProfile(QSurfaceFormat::CoreProfile); format.setDepthBufferSize(24); format.setStencilBufferSize(8);
    format.setAlphaBufferSize(0); format.setSwapInterval(0);
    context.setFormat(format); require(context.create(),"Cannot create native context");
    QOffscreenSurface producer; producer.setFormat(context.format()); producer.create();
    require(producer.isValid(),"Cannot create import surface");
    QWindow window; window.setSurfaceType(QSurface::OpenGLSurface); window.setFormat(context.format());
    window.resize(160,160); window.show();
    QElapsedTimer deadline; deadline.start();
    while(!window.isExposed() && deadline.elapsed()<5000) { QApplication::processEvents(); QThread::msleep(1); }
    require(window.isExposed(),"Native drawable was not exposed");
    auto gpu=std::make_shared<ModelGpuContext>(); gpu->context=&context; gpu->surface=&producer;
    gpu->window=&window; gpu->exposed=true;
    std::unique_ptr<ModelWorker> worker(factory(cfg)); worker->gpuContext(gpu);
    worker->viewport(window.size(),window.devicePixelRatio());
    QString error; require(worker->reload(cfg,error),qPrintable(error));
    require(QOpenGLContext::currentContext()!=&context,"Import retained its offscreen context");
    int rendered=0,presented=0;
    worker->frameRendered=[&] { ++rendered; }; worker->framePresented=[&] { ++presented; };
    worker->present();
    const auto renderer=worker->metrics()["renderer"].toString();
    require(!renderer.isEmpty() && renderer!="unknown" && !renderer.contains("llvmpipe",Qt::CaseInsensitive) &&
        !renderer.contains("softpipe",Qt::CaseInsensitive),"Expected native hardware renderer");
    require(worker->metrics()["frame_transport"]=="native_window","Native worker selected an image transport");
    for(int i=0;i<32;++i)worker->drag({2,.5},false);
    worker->drag({2,-1},true); worker->scroll(120); worker->cameraState();
    require(QOpenGLContext::currentContext()!=&context,"Camera input activated an offscreen context");
    worker->present();
    require(rendered==2 && presented==2,"Native frames did not publish once per draw");
    require(worker->metrics()["capture_readbacks"].toInt()==0,"Live drawing read pixels back");
    const auto camera=worker->cameraState();
    require(!worker->snapshot().isNull(),"Explicit native capture failed");
    require(worker->metrics()["capture_readbacks"].toInt()==1,"Explicit capture did not account for its readback");
    require(rendered==2 && presented==2 && sameCamera(camera,worker->cameraState()),"Capture changed live presentation or camera");
    gpu->exposed=false; worker->exposed(false); worker->present();
    require(rendered==2 && presented==2,"Unexposed view drew a fallback image");
    require(!worker->thumbnailSnapshot({80,50}).isNull(),"Detached departure capture failed");
    gpu->exposed=true; worker->exposed(true); worker->present();
    require(rendered==3 && presented==3,"Native drawing did not recover after capture and re-exposure");
    require(QOpenGLContext::currentContext()!=&context,"Native drawing retained its context");
    std::cout<<"Direct native drawing, CPU-only camera input, capture isolation and re-exposure passed on "<<renderer.toStdString()<<'\n';
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        require(argc==2,"Expected native backend path");
        QLibrary library(QString::fromLocal8Bit(argv[1])); library.setLoadHints(QLibrary::PreventUnloadHint);
        const auto factory=reinterpret_cast<Factory>(preview::resolveBackend(library,"asset_preview_create_model_worker"));
        QTemporaryDir root; require(root.isValid(),"Cannot create model fixture directory");
        QFile source(root.filePath("triangle.obj")); require(source.open(QIODevice::WriteOnly),"Cannot create OBJ");
        source.write("v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n"); source.close();
        QJsonObject cfg{{"path",source.fileName()},{"kind","model"},{"settings",QJsonObject{{"textures",true}}}};
        std::unique_ptr<ModelWorker> worker(factory(cfg)); QString error;
        worker->viewport({128,128},1); worker->exposed(true);
        require(worker->reload(cfg,error),qPrintable(error));
        worker->present();
        require(!worker->snapshot().isNull(),"Native first frame missing");
        const auto renderer=worker->metrics()["renderer"].toString();
        require(!renderer.isEmpty() && renderer!="unknown" && !renderer.contains("llvmpipe",Qt::CaseInsensitive) &&
            !renderer.contains("softpipe",Qt::CaseInsensitive),"Expected hardware renderer");
        int frames=0; worker->frameRendered=Frames{&frames};
        cfg["settings"]=QJsonObject{{"textures",false}};
        worker->settings(cfg); // A successful replacement awaits first-frame notification.
        worker->loaded=[](bool,QString) { throw std::runtime_error("Injected completion failure"); };
        const QJsonValue renders=worker->metrics()["renders"];
        require(!worker->snapshot().isNull(),"Capture invoked a live completion callback");
        require(frames==0 && worker->metrics()["renders"]==renders,"Capture advanced live presentation");
        bool threw=false;
        try { worker->present(); } catch(const std::runtime_error&) { threw=true; }
        require(threw,"The live completion failure was not exercised");
        require(worker->frameRendered.target<Frames>()!=nullptr,"Capture replaced the render callback");
        worker->loaded={}; require(!worker->snapshot().isNull(),"Capture did not recover after an exception");
        worker->viewport({160,160},1); QApplication::processEvents();
        require(frames>0,"Live frame callback did not recover");
        std::cout<<"Headless capture isolation and completion exception recovery passed on "<<renderer.toStdString()<<'\n';
        worker.reset();
        cameraMotion(factory,cfg);
        nativePresentation(factory,cfg);
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
