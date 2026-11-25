#include "model_worker.h"
#include "backend_loader.h"
#include "preview_cache.h"
#include "model_frame.h"
#include "gl_framebuffer.h"
#include "animation_time.h"
#include "preview_navigation.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QHideEvent>
#include <QHash>
#include <QJsonArray>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSet>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <atomic>
#include <deque>
#include <optional>
#include <cmath>

namespace {
class RenderDispatcher final : public QObject {
public:
    RenderDispatcher() {
        tick.setParent(this); tick.setSingleShot(true);
        connect(&tick,&QTimer::timeout,this,[this] { runOne(); if(!jobs.empty())tick.start(1); });
    }
    void enqueue(std::function<void()> job,int priority,const void* key,bool retiring,bool motion,std::function<void()> after) {
        if(retiring)std::erase_if(jobs,[key](const Job& item) { return item.key==key; });
        jobs.push_back({key,priority,motion,std::move(job),std::move(after)});
        if(!tick.isActive())tick.start(1);
    }
    void drain() { tick.stop(); while(!jobs.empty())runOne(); }
private:
    void runOne() {
        if(jobs.empty())return;
        // Input wins over refreshes and new imports. A view's own FIFO stays
        // intact: promote its predecessors together with the requested input.
        QSet<const void*> seen;
        QHash<const void*,int> priorities;
        for(const auto& job:jobs)if(job.key)priorities[job.key]=std::max(priorities.value(job.key),job.priority);
        int choice=0,best=-1;
        for(int at=0;at<int(jobs.size());++at) {
            const auto& head=jobs[at];
            if(head.key && seen.contains(head.key))continue;
            seen.insert(head.key);
            const int priority=head.key?priorities.value(head.key):head.priority;
            if(priority>best) { choice=at; best=priority; }
        }
        auto job=std::move(jobs[choice]); jobs.erase(jobs.begin()+choice);
        auto after=std::move(job.after);
        QElapsedTimer batch; batch.start();
        job.run();
        // A render/readback can take longer than a pointer's event interval.
        // Apply already queued motion before yielding to the scene's frame
        // callback, rather than rendering one stale frame per mouse event.
        // Keep exact deltas/order and stop at any same-view command barrier.
        // The work/time caps preserve service for other views and worker timers.
        if(job.motion && job.key)for(int count=1;count<256 && batch.nsecsElapsed()<4000000;++count) {
            const auto next=std::find_if(jobs.begin(),jobs.end(),[&job](const Job& item) { return item.key==job.key; });
            if(next==jobs.end() || !next->motion)break;
            auto motion=std::move(*next); jobs.erase(next); motion.run(); after=std::move(motion.after);
        }
        if(after)after();
    }
    QTimer tick;
    struct Job { const void* key; int priority; bool motion; std::function<void()> run,after; };
    std::deque<Job> jobs;
};
class RenderRuntime final : public QObject {
public:
    QThread thread;
    RenderDispatcher* dispatcher=new RenderDispatcher;
    explicit RenderRuntime(QObject* owner):QObject(owner) {
        thread.setObjectName("preview-render");
        dispatcher->moveToThread(&thread);
        QObject::connect(&thread,&QThread::finished,dispatcher,&QObject::deleteLater);
        thread.start();
    }
    ~RenderRuntime() override {
        // Views enqueue exact scene destruction before the application tears
        // down its worker. Navigation never waits for an in-progress import.
        QMetaObject::invokeMethod(dispatcher,[this]{dispatcher->drain();},Qt::BlockingQueuedConnection);
        thread.quit(); thread.wait();
    }
};
RenderRuntime& runtime() {
    static auto* instance=new RenderRuntime(qApp);
    return *instance;
}
void renderWork(std::function<void()> job,int priority=0,const void* key=nullptr,bool retiring=false,bool motion=false,std::function<void()> after={}) {
    auto* dispatcher=runtime().dispatcher;
    QMetaObject::invokeMethod(dispatcher,[dispatcher,job=std::move(job),priority,key,retiring,motion,after=std::move(after)]() mutable {
        dispatcher->enqueue(std::move(job),priority,key,retiring,motion,std::move(after));
    },Qt::QueuedConnection);
}
class Observer final : public preview::CacheStore {
public:
    std::function<void(QString,QString,QString,qint64,QString,qint64,QJsonObject)> event;
    std::function<void(QString,QString,QString)> identity;
    preview::CachedPayload take(const QString&,bool) override { return {}; }
    void put(const QString&,preview::CachedPayload) override {}
    void identify(const QString& key,const QString& kind,const QString& path) override { if(identity)identity(key,kind,path); }
    void observe(const QString& tier,const QString& action,const QString& key,qint64 bytes,const QString& reason,qint64 us,const QJsonObject& details) override {
        if(event)event(tier,action,key,bytes,reason,us,details);
    }
};
std::shared_ptr<ModelGpuContext> createGpuContext() {
    if(QGuiApplication::platformName()=="offscreen")return {};
    auto& workerRuntime=runtime();
    auto* context=new QOpenGLContext;
    auto format=QSurfaceFormat::defaultFormat();
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3,3); format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24); format.setStencilBufferSize(8);
    format.setAlphaBufferSize(0); format.setSwapInterval(0);
    context->setFormat(format);
    if(!context->create()) { delete context; throw std::runtime_error("Cannot create model GPU context"); }
    auto* surface=new QOffscreenSurface(nullptr,qApp); surface->setFormat(context->format()); surface->create();
    if(!surface->isValid()) { delete surface; delete context; throw std::runtime_error("Cannot create model producer surface"); }
    context->moveToThread(&workerRuntime.thread);
    return {new ModelGpuContext{context,surface},[](ModelGpuContext* gpu) {
        QMetaObject::invokeMethod(gpu->context,[gpu] {
            gpu->context->doneCurrent(); gpu->context->deleteLater(); gpu->surface->deleteLater(); delete gpu;
        },Qt::QueuedConnection);
    }};
}
struct ModelState {
    std::atomic_bool dead=false;
    std::atomic_uint revision=0;
    std::atomic_uint pendingCommands=0;
    std::atomic_bool renderPending=false;
    std::atomic<quint64> presented=0;
    // The worker and observer are used and destroyed only on the render thread.
    ModelWorker* worker=nullptr;
    Observer observer;
    unsigned activeRevision=0;
    double queueMs=0;
    QString failure;
    QMutex mutex;
    std::shared_ptr<ModelGpuContext> gpu;
    QJsonObject metrics,home;
    std::optional<std::pair<bool,QString>> completion;
    unsigned publishedRevision=0;
    bool notification=false,playbackNotification=false;
};
class ModelSurface final : public QWindow, public PreviewView {
public:
    ModelSurface(QJsonObject cfg,QWidget* parent,LoadedCallback callback,InspectionCallback changed)
        :ModelSurface(std::move(cfg),parent,std::move(callback),std::move(changed),createGpuContext()) {}
    ModelSurface(QJsonObject cfg,QWidget*,LoadedCallback callback,InspectionCallback changed,std::shared_ptr<ModelGpuContext> gpu)
        :state(std::make_shared<ModelState>()),loaded(std::move(callback)),config(std::move(cfg)) {
        state->gpu=std::move(gpu);
        setSurfaceType(QSurface::OpenGLSurface);
        if(state->gpu)setFormat(state->gpu->context->format());
        setMinimumSize({100,100}); setCursor(Qt::OpenHandCursor);
        inspectionChanged=std::move(changed);
        QPointer<ModelSurface> guard(this); const auto slot=state;
        QTimer::singleShot(0,this,[this,guard,slot] {
        renderWork([guard,slot,cfg=config] {
            if(slot->dead)return;
            try {
                // Loading the optional library and plugins belongs to the worker
                // too; the first model must not freeze the shell during dlopen.
                static QLibrary library(QCoreApplication::applicationDirPath()+"/asset-preview-f3d.so");
                library.setLoadHints(QLibrary::PreventUnloadHint);
                using Factory=ModelWorker* (*)(QJsonObject);
                const auto factory=reinterpret_cast<Factory>(preview::resolveBackend(library,"asset_preview_create_model_worker"));
                if(slot->dead)return;
                slot->worker=factory(cfg);
                slot->worker->gpuContext(slot->gpu);
                slot->worker->cache=&slot->observer;
                slot->observer.identity=[guard](QString key,QString kind,QString path) {
                    QMetaObject::invokeMethod(qApp,[guard,key,kind,path] { if(guard && guard->cache)guard->cache->identify(key,kind,path); },Qt::QueuedConnection);
                };
                slot->observer.event=[guard](QString tier,QString action,QString key,qint64 bytes,QString reason,qint64 us,QJsonObject details) {
                    QMetaObject::invokeMethod(qApp,[guard,tier,action,key,bytes,reason,us,details] {
                        if(guard && guard->cache)guard->cache->observe(tier,action,key,bytes,reason,us,details);
                    },Qt::QueuedConnection);
                };
                slot->worker->frameRendered=[guard,slot] { publish(guard,slot,{},false); };
                slot->worker->framePresented=[slot] { ++slot->presented; };
                slot->worker->loaded=[guard,slot](bool ok,QString error) { publish(guard,slot,std::pair{ok,error}); };
                slot->worker->inspectionChanged=[guard,slot](const QJsonObject& camera,bool persist) {
                    const auto version=slot->activeRevision;
                    QMetaObject::invokeMethod(qApp,[guard,slot,version,camera,persist] {
                        if(guard && !slot->dead && version==slot->revision) { guard->statistics["camera"]=camera; if(guard->inspectionChanged)guard->inspectionChanged(camera,persist); }
                    },Qt::QueuedConnection);
                };
                slot->worker->playbackChanged=[guard,slot] { publish(guard,slot); };
                slot->worker->playbackSettingsRequested=[guard,slot](QJsonObject patch) {
                    const auto version=slot->activeRevision;
                    QMetaObject::invokeMethod(qApp,[guard,slot,version,patch] {
                        if(guard && !slot->dead && version==slot->revision && guard->playbackSettingsRequested)guard->playbackSettingsRequested(patch);
                    },Qt::QueuedConnection);
                };
            } catch(const std::exception& error) { slot->failure="3D backend unavailable · "+QString::fromUtf8(error.what()); }
        },false,slot.get());
        });
    }
    ~ModelSurface() override {
        state->dead=true; ++state->revision;
        detachSurface();
        const auto slot=state;
        renderWork([slot] {
            if(slot->worker) {
                slot->worker->frameRendered={}; slot->worker->loaded={};
                slot->worker->framePresented={};
                slot->worker->inspectionChanged={}; slot->worker->playbackChanged={};
                slot->worker->playbackSettingsRequested={};
                delete slot->worker; slot->worker=nullptr;
            }
            slot->gpu.reset();
        },2,slot.get(),true);
    }
    bool reload(const QJsonObject& cfg,QString&) override {
        config=cfg; pending=true; const auto version=++state->revision;
        // Layout settles first, so the initial upload/render uses the actual
        // card size rather than a throwaway 100-pixel constructor viewport.
        QTimer::singleShot(0,this,[this,cfg,version] {
            if(version!=state->revision)return;
            const auto extent=size(); const auto scale=devicePixelRatio(); const bool visible=isVisible();
            post([cfg,extent,scale,visible](ModelWorker& worker) {
            worker.viewport(extent,scale); worker.exposed(visible);
            QString error; const bool ok=worker.reload(cfg,error);
            // First-frame publication includes import, upload and shader work.
            // GPU work and native window presentation stay on the worker.
            worker.present();
            return std::pair{ok,error};
            },version,true);
        });
        return true;
    }
    void settings(const QJsonObject& cfg) override {
        const auto old=config["settings"].toObject(),next=cfg["settings"].toObject();
        const bool reload=preview::frameChanged(old,next) || next["shape"]!=old["shape"] || next["up_axis"]!=old["up_axis"] ||
            next["animation"].toInt()!=old["animation"].toInt() ||
            next["materials"].toBool(true)!=old["materials"].toBool(true) || next["textures"].toBool(true)!=old["textures"].toBool(true) ||
            next["texture_quality"].toString("original")!=old["texture_quality"].toString("original") ||
            next["ovoxel_mode"].toString("surface")!=old["ovoxel_mode"].toString("surface") ||
            next["edge_topology"].toString("source")!=old["edge_topology"].toString("source");
        config=cfg;
        if(reload && statistics["engine"].toBool()) { pending=true; if(loadingChanged)loadingChanged(); }
        command([cfg](ModelWorker& worker){worker.settings(cfg);});
    }
    void fit() override { command([](ModelWorker& worker){worker.fit();}); }
    void zoomBy(double factor) {
        const double delta=std::log(factor)/std::log(1.0015);
        command([delta](ModelWorker& worker){worker.scroll(delta);},true);
    }
    void panBy(QPointF viewportDelta) {
        const QPointF pixels(viewportDelta.x()*width(),viewportDelta.y()*height());
        command([pixels](ModelWorker& worker){worker.drag(pixels,true);if(worker.inspectionChanged)worker.inspectionChanged(worker.cameraState(),true);},true);
    }
    void setPanMode(bool enabled) {panOnly=enabled;setCursor(enabled?Qt::OpenHandCursor:Qt::ArrowCursor);}
    void resetView() override { command([](ModelWorker& worker){worker.resetView();}); }
    QJsonObject metrics() const override {
        auto result=statistics; result["loading"]=pending; result["render_worker_limit"]=1;
        result["command_pending"]=state->pendingCommands>0 || state->renderPending || statistics["render_pending"].toBool();
        result["presented_frames"]=qint64(state->presented.load()); result["presented_serial"]=qint64(state->presented.load());
        result["display"]=QJsonObject{{"visible",isVisible()},{"exposed",isExposed()},{"valid",handle()!=nullptr},{"window_id",handle()?qint64(winId()):0},
            {"width",width()},{"height",height()},{"parent",parent()?parent()->objectName():QString()},
            {"sharing",false}};
        result["display_readbacks"]=qint64(readbacks); result["display_readback_bytes"]=qint64(readbackBytes); return result;
    }
    bool loading() const override { return pending; }
    QImage snapshot() override { return captureFrame({}); }
    QImage thumbnailSnapshot(QSize bound) override { return captureFrame(bound); }
    QImage captureFrame(QSize bound) {
        if(pending)return {};
        QImage result; const auto slot=state;
        QMetaObject::invokeMethod(runtime().dispatcher,[slot,bound,&result] {
            if(!slot->worker)return;
            try { result=bound.isValid()?slot->worker->thumbnailSnapshot(bound):slot->worker->snapshot(); }
            catch(const std::exception& error) { qWarning("Model capture failed: %s",error.what()); }
        },Qt::BlockingQueuedConnection);
        if(!result.isNull()) { ++readbacks; readbackBytes+=result.sizeInBytes(); }
        return result;
    }
    void requestThumbnail(QSize bound,std::function<void(QImage)> ready) override {
        const auto slot=state;
        // A departure capture must not make navigation wait behind another import.
        // Keep it ahead of this scene's queued destruction without retaining a window.
        renderWork([slot,bound,ready=std::move(ready)] {
            QImage result;
            if(slot->worker)try { result=slot->worker->thumbnailSnapshot(bound); }
            catch(const std::exception& error) { qWarning("Model thumbnail failed: %s",error.what()); }
            QMetaObject::invokeMethod(qApp,[ready,result] { ready(result); },Qt::QueuedConnection);
        },2);
    }
    QJsonObject cameraState() const override { return statistics["camera"].toObject(); }
    QJsonObject cameraHome() const override { return home; }
    void setCameraState(const QJsonObject& camera) override { command([camera](ModelWorker& worker){worker.setCameraState(camera);},true); }
    void cameraView(const QString& view) override { command([view](ModelWorker& worker){worker.cameraView(view);}); }
    bool seek(double seconds,QString& error) override {
        if(!statistics["seekable"].toBool()) { error="This model has no seekable animation"; return false; }
        command([seconds](ModelWorker& worker){QString error; if(!worker.seek(seconds,error))qWarning("Seek failed: %s",qPrintable(error));}); return true;
    }
    bool stepAnimation(int direction,int fps,QString& error) override {
        if(!statistics["seekable"].toBool()) { error="This model has no seekable animation"; return false; }
        const auto cfg=config;
        command([cfg,direction,fps](ModelWorker& worker) {
            worker.settings(cfg);
            const auto state=worker.metrics();
            const auto at=preview::animationStep(state["position"].toDouble(),state["duration"].toDouble(),fps,direction);
            QString error; if(!worker.seek(at,error))qWarning("Frame step failed: %s",qPrintable(error));
        });
        return true;
    }
protected:
    bool event(QEvent* event) override {
        if(event->type()==QEvent::PlatformSurface) {
            auto* surface=static_cast<QPlatformSurfaceEvent*>(event);
            if(surface->surfaceEventType()==QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)detachSurface();
        }
        return QWindow::event(event);
    }
    void resizeEvent(QResizeEvent* event) override {
        QWindow::resizeEvent(event); const auto extent=size(); const auto scale=devicePixelRatio();
        command([extent,scale](ModelWorker& worker){worker.viewport(extent,scale);});
    }
    void exposeEvent(QExposeEvent* event) override {
        QWindow::exposeEvent(event); const bool shown=isExposed();
        if(state->gpu) {
            QMutexLocker lock(&state->gpu->mutex);
            state->gpu->window=this; state->gpu->exposed=shown;
        }
        command([shown](ModelWorker& worker){worker.exposed(shown);});
    }
    void wheelEvent(QWheelEvent* event) override { const auto delta=event->angleDelta().y()?event->angleDelta().y():event->pixelDelta().y(); const auto point=event->position(); command([delta,point](ModelWorker& worker){worker.pointer(point);worker.scroll(delta);},true); event->accept(); }
    void mousePressEvent(QMouseEvent* event) override { last=event->position(); const auto point=last; command([point](ModelWorker& worker){worker.pointer(point);},true);setCursor(Qt::ClosedHandCursor); if(activated)activated(); requestActivate(); }
    void mouseReleaseEvent(QMouseEvent* event) override {
        setCursor(panOnly?Qt::OpenHandCursor:Qt::ArrowCursor);
        command([](ModelWorker& worker){if(worker.inspectionChanged)worker.inspectionChanged(worker.cameraState(),true);},true); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        const auto delta=event->position()-last; last=event->position();
        if(event->buttons()==Qt::NoButton)return;
        const bool pan=panOnly || event->buttons().testFlag(Qt::MiddleButton) || event->buttons().testFlag(Qt::RightButton) || event->modifiers().testFlag(Qt::ShiftModifier);
        command([delta,pan](ModelWorker& worker){worker.drag(delta,pan);},true);
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { fit(); }
private:
    void detachSurface() {
        if(!state->gpu)return;
        QMutexLocker lock(&state->gpu->mutex);
        state->gpu->exposed=false; state->gpu->window=nullptr;
    }
    template<class Action> void command(Action action,bool motion=false) {
        post([action](ModelWorker& worker){action(worker); return std::pair{true,QString()};},state->revision,false,motion);
    }
    template<class Action> void post(Action action,unsigned version,bool completion,bool motion=false) {
        QPointer<ModelSurface> guard(this); const auto slot=state;
        ++slot->pendingCommands;
        QElapsedTimer queued; queued.start();
        const int priority=statistics["engine"].toBool()?(completion?1:2):0;
        renderWork([guard,slot,version,completion,action,queued,motion] {
            const auto finished=qScopeGuard([slot] { --slot->pendingCommands; });
            if(slot->dead || version!=slot->revision)return;
            slot->activeRevision=version;
            if(completion)slot->queueMs=queued.nsecsElapsed()/1e6;
            if(!slot->worker) { if(completion)publish(guard,slot,std::pair{false,slot->failure}); return; }
            try {
                const auto result=action(*slot->worker);
                slot->renderPending=slot->worker->renderPending();
                if(!motion)publish(guard,slot,completion?std::optional(result):std::nullopt);
            } catch(const std::exception& error) { publish(guard,slot,std::pair{false,QString::fromUtf8(error.what())}); }
        },priority,slot.get(),false,motion,motion?std::function<void()>([guard,slot,version] {
            if(!slot->dead && version==slot->revision)publish(guard,slot,{},false);
        }):std::function<void()>{});
    }
    static void publish(QPointer<ModelSurface> guard,const std::shared_ptr<ModelState>& slot,std::optional<std::pair<bool,QString>> completion={},bool playbackNotification=true) {
        if(slot->dead || slot->activeRevision!=slot->revision)return;
        QJsonObject metrics,home;
        if(slot->worker) {
            // Camera reads can commit an orbit and recalculate scene clipping.
            // Keep native work outside the mailbox lock shared with the GUI.
            metrics=slot->worker->metrics(); home=slot->worker->cameraHome();
            auto timing=metrics["load_timing"].toObject(); timing["queue_ms"]=slot->queueMs;
            metrics["load_timing"]=timing;
        }
        QMutexLocker lock(&slot->mutex);
        if(slot->dead || slot->activeRevision!=slot->revision)return;
        if(slot->publishedRevision!=slot->activeRevision) {
            slot->completion.reset(); slot->playbackNotification=false;
        }
        if(slot->worker) {
            slot->renderPending=slot->worker->renderPending();
            slot->metrics=std::move(metrics); slot->home=std::move(home);
        }
        if(completion)slot->completion=completion;
        slot->playbackNotification|=playbackNotification;
        slot->publishedRevision=slot->activeRevision;
        if(slot->notification)return;
        slot->notification=true;
        QMetaObject::invokeMethod(qApp,[guard,slot] {
            QJsonObject metrics,home; std::optional<std::pair<bool,QString>> completion; unsigned version; bool playbackNotification;
            {
                QMutexLocker lock(&slot->mutex); metrics=slot->metrics; home=slot->home;
                completion=std::move(slot->completion); slot->completion.reset(); version=slot->publishedRevision; slot->notification=false;
                playbackNotification=slot->playbackNotification; slot->playbackNotification=false;
            }
            const auto current=[&] { return guard && !slot->dead && version==slot->revision; };
            if(!current())return;
            guard->statistics=metrics; guard->home=home;
            const auto presented=slot->presented.load();
            if(presented!=guard->notifiedPresentation) {
                guard->notifiedPresentation=presented;
                const auto callback=guard->framePresented;
                if(callback)callback();
            }
            // Notifications may navigate away or reload the view synchronously.
            if(!current())return;
            if(completion) {
                guard->pending=false;
                const auto callback=guard->loaded;
                if(callback)callback(completion->first,completion->second);
            }
            if(current() && playbackNotification) {
                const auto callback=guard->playbackChanged;
                if(callback)callback();
            }
        },Qt::QueuedConnection);
    }
    std::shared_ptr<ModelState> state;
    LoadedCallback loaded;
    QJsonObject config,statistics,home;
    quint64 notifiedPresentation=0,readbacks=0,readbackBytes=0;
    QPointF last;
    bool pending=false;
    bool panOnly=false;
};
}
bool preview::zoomView(PreviewView* view,double factor) {
    if(!std::isfinite(factor) || factor<0.1 || factor>10)return false;
    if(auto* image=dynamic_cast<ImageView*>(view)){image->zoomBy(factor,QPointF(image->width()/2.0,image->height()/2.0));return true;}
    if(auto* model=dynamic_cast<ModelSurface*>(view)){model->zoomBy(factor);return true;}
    return false;
}
bool preview::panView(PreviewView* view,QPointF delta) {
    if(!std::isfinite(delta.x()) || !std::isfinite(delta.y()) || std::abs(delta.x())>1 || std::abs(delta.y())>1)return false;
    if(auto* image=dynamic_cast<ImageView*>(view)){image->panBy(delta);return true;}
    if(auto* model=dynamic_cast<ModelSurface*>(view)){model->panBy(delta);return true;}
    return false;
}
bool preview::setPanMode(PreviewView* view,bool enabled) {
    if(auto* model=dynamic_cast<ModelSurface*>(view)){model->setPanMode(enabled);return true;}
    return dynamic_cast<ImageView*>(view)!=nullptr;
}
ModelHandle createModel(QJsonObject config,QWidget* parent,LoadedCallback loaded,InspectionCallback changed) {
    auto* surface=new ModelSurface(std::move(config),parent,std::move(loaded),std::move(changed));
    auto* container=QWidget::createWindowContainer(surface,parent);
    container->setMinimumSize(100,100); container->setFocusPolicy(Qt::StrongFocus);
    return {container,surface};
}
