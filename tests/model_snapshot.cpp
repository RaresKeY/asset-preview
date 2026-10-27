#include "model_worker.h"
#include "backend_loader.h"
#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

struct Frames { int* count; void operator()(QImage) const { ++*count; } };
static void require(bool ok,const char* error) { if(!ok)throw std::runtime_error(error); }
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        require(argc==2,"Expected native backend path");
        QLibrary library(QString::fromLocal8Bit(argv[1])); library.setLoadHints(QLibrary::PreventUnloadHint);
        using Factory=ModelWorker* (*)(QJsonObject);
        const auto factory=reinterpret_cast<Factory>(preview::resolveBackend(library,"asset_preview_create_model_worker"));
        QTemporaryDir root; require(root.isValid(),"Cannot create model fixture directory");
        QFile source(root.filePath("triangle.obj")); require(source.open(QIODevice::WriteOnly),"Cannot create OBJ");
        source.write("v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n"); source.close();
        QJsonObject cfg{{"path",source.fileName()},{"kind","model"},{"settings",QJsonObject{{"textures",true}}}};
        std::unique_ptr<ModelWorker> worker(factory(cfg)); QString error;
        worker->viewport({128,128},1); worker->exposed(true);
        require(worker->reload(cfg,error),qPrintable(error));
        require(!worker->snapshot().isNull(),"Native first frame missing");
        const auto renderer=worker->metrics()["renderer"].toString();
        require(!renderer.isEmpty() && renderer!="unknown" && !renderer.contains("llvmpipe",Qt::CaseInsensitive) &&
            !renderer.contains("softpipe",Qt::CaseInsensitive),"Expected hardware renderer");
        int frames=0; worker->frameReady=Frames{&frames};
        cfg["settings"]=QJsonObject{{"textures",false}};
        worker->settings(cfg); // A successful replacement awaits first-frame notification.
        worker->loaded=[](bool,QString) { throw std::runtime_error("Injected completion failure"); };
        bool threw=false;
        try { worker->snapshot(); } catch(const std::runtime_error&) { threw=true; }
        require(threw,"The render/capture failure was not exercised");
        require(worker->frameReady.target<Frames>()!=nullptr,"Capture left its expired stack callback installed");
        worker->loaded={}; require(!worker->snapshot().isNull(),"Capture did not recover after an exception");
        worker->viewport({160,160},1); QApplication::processEvents();
        require(frames>0,"Live frame callback did not recover");
        std::cout<<"Capture exception recovery passed on "<<renderer.toStdString()<<'\n';
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
