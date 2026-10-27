#include "../src/viewer.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibrary>
#include <QSurfaceFormat>
#include <QSet>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using Factory=ModelHandle(*)(QJsonObject,QWidget*,LoadedCallback,InspectionCallback,InspectionEventCallback);

int main(int argc,char** argv) {
    QSurfaceFormat format;
    format.setVersion(3,3); format.setProfile(QSurfaceFormat::CoreProfile); format.setDepthBufferSize(24); format.setStencilBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv);
    if(argc!=4 && argc!=5) { std::fprintf(stderr,"Usage: inspection-vtk-smoke MODULE MODEL OUTPUT_DIR [--generic]\n"); return 2; }
    const bool generic=argc==5 && QString::fromLocal8Bit(argv[4])=="--generic";
    QLibrary library(QString::fromLocal8Bit(argv[1])); library.setLoadHints(QLibrary::PreventUnloadHint);
    auto factory=reinterpret_cast<Factory>(library.resolve("asset_preview_create_inspection"));
    if(!factory) { std::fprintf(stderr,"%s\n",qPrintable(library.errorString())); return 2; }
    const QString output=QString::fromLocal8Bit(argv[3]); QDir().mkpath(output);
    QWidget window; window.resize(800,600);
    QJsonObject config{{"kind","model"},{"path",QString::fromLocal8Bit(argv[2])},{"settings",QJsonObject{{"grid",false},{"lighting","lightkit"}}}};
    ModelHandle handle{};
    bool ready=false; QString failure;
    handle=factory(config,&window,[&](bool ok,QString error) { ready=ok; failure=error; },{},{});
    handle.surface->setGeometry(window.rect()); window.show();
    QTimer::singleShot(1000,&app,[&] {
        auto check=[&](bool condition,const char* message) { if(!condition) { std::fprintf(stderr,"FAIL: %s (%s)\n",message,qPrintable(failure)); std::exit(1); } };
        check(ready,"visible load");
        QString error;
        auto command=[&](QJsonObject request) { error.clear(); auto result=handle.view->inspect(request,error); check(error.isEmpty(),qPrintable(error)); return result; };
        const auto state=command({{"action","state"}});
        if(generic) {
            QJsonArray allParts; int offset=0;
            for(;;) {
                const auto page=command({{"action","parts"},{"offset",offset}});
                const auto rows=page["items"].toArray();
                for(const auto& row:rows) allParts.append(row);
                offset+=int(rows.size());
                if(!page["has_more"].toBool() || rows.isEmpty()) break;
            }
            check(handle.view->snapshot().save(output+"/model.png"),"requested capture");
            const auto metrics=handle.view->metrics();
            QFile result(output+"/proof.json"); check(result.open(QIODevice::WriteOnly),"proof output opens");
            result.write(QJsonDocument(QJsonObject{{"state",state},{"parts",allParts},{"metrics",metrics}}).toJson()); result.close();
            std::printf("PARTS %d MAPPED %d UNMAPPED %d RENDERER %s\n",int(allParts.size()),state["mapped_actors"].toInt(),state["unmapped_actors"].toInt(),qPrintable(metrics["renderer"].toString()));
            delete handle.surface; handle={}; app.quit(); return;
        }
        const auto parts=command({{"action","parts"}})["items"].toArray();
        check(parts.size()==4,"four source parts, multi-primitive node stays one part");
        check(state["mapped_actors"].toInt()==5 && state["unmapped_actors"].toInt()==0,"five exact actor mappings");
        QMap<int,QString> ids;
        for(const auto& p:parts) { auto part=p.toObject(); check(part["mapped"].toBool(),"all fixture parts mapped"); ids[part["node_index"].toInt()]=part["id"].toString(); }
        const auto initialHome=handle.view->cameraHome();
        const int initialLoads=handle.view->metrics()["loads"].toInt();
        handle.view->setCameraState({{"position",QJsonArray{0,.2,12}},{"focal",QJsonArray{0,.2,0}},{"up",QJsonArray{0,1,0}},{"angle",45},{"parallel_span",5}});
        const auto adjustedCamera=handle.view->cameraState();
        auto displayConfig=config,displaySettings=config["settings"].toObject(); displaySettings["edges"]=true; displayConfig["settings"]=displaySettings;
        handle.view->settings(displayConfig);
        check(handle.view->cameraState()==adjustedCamera && handle.view->cameraHome()==initialHome,"same-frame display settings preserve camera and home");
        auto frameConfig=displayConfig; displaySettings["up_axis"]="z"; displaySettings["front_axis"]="-y"; frameConfig["settings"]=displaySettings;
        handle.view->settings(frameConfig);
        const auto framed=handle.view->cameraState();
        const auto hasNewFrame=[](const QJsonObject& camera) {
            const auto p=camera["position"].toArray(),f=camera["focal"].toArray();
            const double x=p[0].toDouble()-f[0].toDouble(),y=p[1].toDouble()-f[1].toDouble(),z=p[2].toDouble()-f[2].toDouble();
            return camera["up"].toArray()==QJsonArray{0,0,1} && x<0 && y<0 && z>0 && std::abs(x-y)<1e-6 && std::abs(x+z)<1e-6;
        };
        check(hasNewFrame(framed) && hasNewFrame(handle.view->cameraHome()) && framed["angle"]==initialHome["angle"] && handle.view->metrics()["loads"].toInt()==initialLoads,
            "frame changes refit camera and home without reimporting geometry");
        handle.view->setCameraState(adjustedCamera); handle.view->resetView();
        check(handle.view->cameraState()==framed,"Reset uses the changed frame's fitted default camera");
        handle.view->settings(config);
        handle.view->setCameraState({{"position",QJsonArray{0,.2,12}},{"focal",QJsonArray{0,.2,0}},{"up",QJsonArray{0,1,0}},{"angle",30},{"parallel_span",5}});
        check(handle.view->snapshot().save(output+"/fixture.png"),"requested capture");
        QSet<int> hitNodes; QJsonObject nozzle;
        for(int y=40;y<600;y+=20) for(int x=40;x<800;x+=20) {
            const auto hit=command({{"action","pick"},{"x",x},{"y",y}})["selection"].toObject();
            if(!hit.isEmpty()) { hitNodes.insert(hit["node_index"].toInt(-1)); if(hit["node_index"].toInt()==8) nozzle=hit; }
        }
        check(hitNodes==QSet<int>{2,4,5,8},"logical-pixel picks resolve each exact source part");
        const auto anchor=nozzle["anchor"].toObject();
        const auto point=anchor["position_local"].toArray(),normal=anchor["normal_local"].toArray();
        check(point.size()==3 && std::abs(point[0].toDouble())<=.40001 && std::abs(point[1].toDouble())<=.30001 && std::abs(point[2].toDouble())<=.85001,"nested nonuniform transform returns component-local point");
        double length=0; for(const auto& v:normal) length+=v.toDouble()*v.toDouble();
        check(std::abs(length-1)<1e-6,"component-local normal is unit length");
        command({{"action","select"},{"target",ids[2]}});
        check(command({{"action","hide"}})["hidden"].toArray().size()==1,"hide selection");
        check(command({{"action","restore"},{"last",true}})["hidden"].toArray().isEmpty(),"visibility undo");
        check(command({{"action","isolate"},{"target",ids[8]}})["hidden"].toArray().size()==3,"isolate exact component");
        command({{"action","restore"},{"all",true}});
        command({{"action","cut"},{"axis","x"},{"position",0},{"keep","positive"},{"enabled",true}});
        bool negativeHit=false;
        for(int y=40;y<600;y+=30) for(int x=40;x<400;x+=30)
            if(command({{"action","pick"},{"x",x},{"y",y}})["selection"].toObject()["node_index"].toInt(-1)==2) negativeHit=true;
        check(!negativeHit,"cell picker honors mapper clipping planes");
        command({{"action","hide"},{"target",ids[4]}});
        command({{"action","markers"},{"items",QJsonArray{QJsonObject{{"id","pin-test"},{"targets",QJsonArray{nozzle}}}}}});
        check(handle.view->snapshot().save(output+"/cut-and-pin.png"),"cut and marker capture");
        auto projection=command({{"action","state"}}); projection["orthographic"]=true;
        projection["camera"]=QJsonObject{{"position",QJsonArray{0,.2,12}},{"focal",QJsonArray{0,.2,0}},{"up",QJsonArray{0,1,0}},{"angle",30},{"parallel_span",5}};
        check(command({{"action","apply"},{"view",projection}})["orthographic"].toBool(),"saved view restores orthographic projection");
        error.clear(); handle.view->inspect({{"action","cut"},{"axis","x"},{"position",0},{"enabled","yes"}},error);
        check(!error.isEmpty(),"wrong clipping enabled type is rejected");
        const auto assertRejectedApply=[&](const QJsonValue& view) {
            const auto prior=command({{"action","state"}});
            error.clear(); handle.view->inspect({{"action","apply"},{"view",view}},error);
            check(!error.isEmpty(),"invalid saved view is rejected");
            check(command({{"action","state"}})==prior,"rejected apply preserves all visible state");
        };
        auto stale=command({{"action","state"}});
        stale["revision"]=QString(64,'0'); stale["hidden"]=QJsonArray{};
        assertRejectedApply(stale);
        stale=command({{"action","state"}});
        stale["dependency_revision"]=QString(64,'0'); stale["hidden"]=QJsonArray{};
        assertRejectedApply(stale);
        assertRejectedApply("invalid view");
        for(const auto& malformed:{QJsonObject{{"cuts","invalid"}},QJsonObject{{"hidden","invalid"}},
            QJsonObject{{"selected",42}},QJsonObject{{"selected",QJsonObject{{"id",42}}}},
            QJsonObject{{"camera","invalid"}},QJsonObject{{"selection","invalid"}},QJsonObject{{"orthographic","invalid"}}})
            assertRejectedApply(malformed);
        auto badCamera=projection["camera"].toObject(); badCamera["angle"]="invalid";
        assertRejectedApply(QJsonObject{{"camera",badCamera}});
        badCamera=projection["camera"].toObject(); badCamera["parallel_span"]="invalid";
        assertRejectedApply(QJsonObject{{"camera",badCamera}});
        badCamera=projection["camera"].toObject(); badCamera["position"]=QJsonArray{2e12,0,12};
        assertRejectedApply(QJsonObject{{"camera",badCamera}});
        badCamera=projection["camera"].toObject(); badCamera["parallel_span"]=2e12;
        assertRejectedApply(QJsonObject{{"camera",badCamera}});
        assertRejectedApply(QJsonObject{{"cuts",QJsonArray{QJsonObject{{"axis","x"},{"position",2e12}}}}});
        auto anchored=command({{"action","state"}}); anchored["selected"]=ids[8]; anchored["selection"]=nozzle;
        auto badSelection=nozzle,badAnchor=nozzle["anchor"].toObject(); badAnchor["kind"]="unsupported"; badSelection["anchor"]=badAnchor;
        auto malformedAnchor=anchored; malformedAnchor["selection"]=badSelection;
        assertRejectedApply(malformedAnchor);
        badAnchor=nozzle["anchor"].toObject(); badAnchor["position_local"]="invalid"; badSelection["anchor"]=badAnchor;
        malformedAnchor["selection"]=badSelection; assertRejectedApply(malformedAnchor);
        badAnchor=nozzle["anchor"].toObject(); badAnchor["normal_local"]=QJsonArray{0,0,0}; badSelection["anchor"]=badAnchor;
        malformedAnchor["selection"]=badSelection; assertRejectedApply(malformedAnchor);
        badAnchor=nozzle["anchor"].toObject(); badAnchor["position_local"]=QJsonArray{2e12,0,0}; badSelection["anchor"]=badAnchor;
        malformedAnchor["selection"]=badSelection; assertRejectedApply(malformedAnchor);
        auto largeAnchor=nozzle["anchor"].toObject(); largeAnchor["extra"]=QString(300*1024,'x');
        auto largeSelection=nozzle; largeSelection["anchor"]=largeAnchor; anchored["selection"]=largeSelection;
        const auto bounded=command({{"action","apply"},{"view",anchored}});
        check(bounded["selection"].toObject()["anchor"].toObject().size()==3 && QJsonDocument(bounded).toJson(QJsonDocument::Compact).size()<256*1024,
            "restored anchors retain only bounded surface coordinates and normal");
        auto extendedCut=bounded; extendedCut["cuts"]=QJsonArray{QJsonObject{{"axis","x"},{"position",0},{"extra",QString(300*1024,'x')}}};
        const auto boundedCuts=command({{"action","apply"},{"view",extendedCut}});
        check(boundedCuts["cuts"].toArray()[0].toObject()==QJsonObject{{"axis","x"},{"position",0},{"keep","positive"},{"enabled",true}} &&
            QJsonDocument(boundedCuts).toJson(QJsonDocument::Compact).size()<256*1024,"restored cuts retain only bounded supported plane fields");
        command({{"action","restore"},{"all",true}});
        command({{"action","select"},{"target",ids[8]}});
        for(int i=0;i<100;++i) command({{"action","hide"}});
        stale=command({{"action","state"}}); stale["revision"]=QString(64,'0'); assertRejectedApply(stale);
        for(int i=0;i<100;++i) command({{"action","restore"},{"last",true}});
        check(command({{"action","state"}})["hidden"].toArray().isEmpty(),"rejected apply preserves a full undo history");
        command({{"action","restore"},{"all",true}});
        command({{"action","hide"}});
        const auto transactional=command({{"action","state"}});
        auto visible=transactional; visible["hidden"]=QJsonArray{};
        command({{"action","apply"},{"view",visible},{"record_undo",false}});
        command({{"action","apply"},{"view",transactional},{"record_undo",false}});
        check(command({{"action","restore"},{"last",true}})["hidden"].toArray().isEmpty(),"rolled-back internal apply preserves existing undo history");
        command({{"action","hide"}});
        command({{"action","apply"},{"view",visible},{"record_undo",false}});
        command({{"action","commit_hidden"},{"hidden",transactional["hidden"]}});
        check(command({{"action","restore"},{"last",true}})["hidden"]==transactional["hidden"],"successful internal apply commits one prior visibility state");
        check(command({{"action","restore"},{"last",true}})["hidden"].toArray().isEmpty(),"committed apply preserves older undo history");
        command({{"action","apply"},{"view",projection}});
        const auto before=command({{"action","state"}});
        auto invalid=config; invalid["path"]="/nonexistent/asset-preview-malformed.glb";
        check(!handle.view->reload(invalid,error),"failed candidate rejected");
        check(command({{"action","state"}})["revision"]==before["revision"],"failed reload retains displayed revision");
        error.clear(); check(handle.view->reload(config,error),"same-revision reload succeeds");
        const auto reloaded=command({{"action","state"}});
        check(reloaded["hidden"]==before["hidden"] && reloaded["cuts"]==before["cuts"],"reload preserves exact visibility and cuts");
        const auto metrics=handle.view->metrics();
        const auto renderer=metrics["renderer"].toString().toLower();
        check(!renderer.isEmpty() && !renderer.contains("llvmpipe") && !renderer.contains("softpipe") && !renderer.contains("software"),"actual hardware OpenGL renderer");
        QFile result(output+"/proof.json"); check(result.open(QIODevice::WriteOnly),"proof output opens");
        result.write(QJsonDocument(QJsonObject{{"state",before},{"parts",parts},{"nozzle_hit",nozzle},{"metrics",metrics}}).toJson()); result.close();
        QTimer::singleShot(250,&app,[&,before] {
            const auto settled=handle.view->metrics();
            QTimer::singleShot(250,&app,[&,settled,before] {
                if(handle.view->metrics()["renders"]!=settled["renders"]) { std::fprintf(stderr,"FAIL: idle render count changed\n"); std::exit(1); }
                delete handle.surface; handle={};
                auto reopenedConfig=config; reopenedConfig["inspection"]=QJsonObject{{"view",before}};
                ready=false;
                handle=factory(reopenedConfig,&window,[&](bool ok,QString error) { ready=ok; failure=error; },{},{});
                handle.surface->setGeometry(window.rect()); handle.surface->show();
                QTimer::singleShot(250,&app,[&,before] {
                    QString error;
                    const auto reopened=handle.view->inspect({{"action","state"}},error);
                    if(!ready || reopened["hidden"]!=before["hidden"] || reopened["cuts"]!=before["cuts"]) {
                        std::fprintf(stderr,"FAIL: reentry did not restore inspection.view (%s)\n",qPrintable(failure)); std::exit(1);
                    }
                    delete handle.surface; handle={};
                    std::puts("PASS: exact hierarchy mapping, picks, transforms, visibility undo, cut-aware picks, pins, last-good reload, idle, capture, unload and reentry");
                    app.quit();
                });
            });
        });
    });
    QTimer::singleShot(15000,&app,[&] { std::fprintf(stderr,"FAIL: deadline\n"); app.exit(1); });
    return app.exec();
}
