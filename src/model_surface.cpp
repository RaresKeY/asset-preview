#include "model_worker.h"
#include "backend_loader.h"
#include "preview_cache.h"
#include "model_frame.h"
#include "animation_time.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QHideEvent>
#include <QJsonArray>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSet>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include <atomic>
#include <deque>
#include <optional>

namespace {
class RenderDispatcher final : public QObject {
public:
    RenderDispatcher() {
        tick.setParent(this); tick.setSingleShot(true);
        connect(&tick,&QTimer::timeout,this,[this] { runOne(); if(!jobs.empty())tick.start(1); });
    }
    void enqueue(std::function<void()> job,int priority,const void* key,bool retiring) {
        if(retiring)std::erase_if(jobs,[key](const Job& item) { return item.key==key; });
        jobs.push_back({key,priority,std::move(job)});
        if(!tick.isActive())tick.start(1);
    }
    void drain() { tick.stop(); while(!jobs.empty())runOne(); }
private:
    void runOne() {
        if(jobs.empty())return;
        // Input wins over refreshes and new imports. A view's own FIFO stays
        // intact: promote its predecessors together with the requested input.
        QSet<const void*> seen;
        int choice=0,best=-1;
        for(int at=0;at<int(jobs.size());++at) {
            const auto& head=jobs[at];
            if(head.key && seen.contains(head.key))continue;
            seen.insert(head.key);
            int priority=head.priority;
            if(head.key)for(const auto& item:jobs)if(item.key==head.key)priority=std::max(priority,item.priority);
            if(priority>best) { choice=at; best=priority; }
        }
        auto job=std::move(jobs[choice]); jobs.erase(jobs.begin()+choice); job.run();
    }
    QTimer tick;
    struct Job { const void* key; int priority; std::function<void()> run; };
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
void renderWork(std::function<void()> job,int priority=0,const void* key=nullptr,bool retiring=false) {
    auto* dispatcher=runtime().dispatcher;
    QMetaObject::invokeMethod(dispatcher,[dispatcher,job=std::move(job),priority,key,retiring]() mutable {
        dispatcher->enqueue(std::move(job),priority,key,retiring);
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
struct ModelState {
    std::atomic_bool dead=false;
    std::atomic_uint revision=0;
    std::atomic_uint pendingCommands=0;
    // The worker and observer are used and destroyed only on the render thread.
    ModelWorker* worker=nullptr;
    Observer observer;
    unsigned activeRevision=0;
    double queueMs=0;
    QString failure;
    QMutex mutex;
    QImage frame;
    QJsonObject metrics,home;
    std::optional<std::pair<bool,QString>> completion;
    unsigned publishedRevision=0;
    bool notification=false;
};
class ModelSurface final : public QWidget, public PreviewView {
public:
    ModelSurface(QJsonObject cfg,QWidget* parent,LoadedCallback callback,InspectionCallback changed)
        :QWidget(parent),state(std::make_shared<ModelState>()),loaded(std::move(callback)),config(std::move(cfg)) {
        setMinimumSize(100,100); setFocusPolicy(Qt::StrongFocus); setCursor(Qt::OpenHandCursor);
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
                slot->worker->cache=&slot->observer;
                slot->observer.identity=[guard](QString key,QString kind,QString path) {
                    QMetaObject::invokeMethod(qApp,[guard,key,kind,path] { if(guard && guard->cache)guard->cache->identify(key,kind,path); },Qt::QueuedConnection);
                };
                slot->observer.event=[guard](QString tier,QString action,QString key,qint64 bytes,QString reason,qint64 us,QJsonObject details) {
                    QMetaObject::invokeMethod(qApp,[guard,tier,action,key,bytes,reason,us,details] {
                        if(guard && guard->cache)guard->cache->observe(tier,action,key,bytes,reason,us,details);
                    },Qt::QueuedConnection);
                };
                slot->worker->frameReady=[guard,slot](QImage frame) { publish(guard,slot,std::move(frame)); };
                slot->worker->loaded=[guard,slot](bool ok,QString error) { publish(guard,slot,{},std::pair{ok,error}); };
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
        const auto slot=state;
        renderWork([slot] {
            if(slot->worker) {
                slot->worker->frameReady={}; slot->worker->loaded={};
                slot->worker->inspectionChanged={}; slot->worker->playbackChanged={};
                slot->worker->playbackSettingsRequested={};
                delete slot->worker; slot->worker=nullptr;
            }
        },2,slot.get(),true);
    }
    bool reload(const QJsonObject& cfg,QString&) override {
        config=cfg; pending=true; const auto version=++state->revision;
        // Layout settles first, so the initial upload/render uses the actual
        // card size rather than a throwaway 100-pixel constructor viewport.
        QTimer::singleShot(0,this,[this,cfg,version] {
            if(version!=state->revision)return;
            const auto extent=size(); const auto scale=devicePixelRatioF(); const bool visible=isVisible();
            post([cfg,extent,scale,visible](ModelWorker& worker) {
            worker.viewport(extent,scale); worker.exposed(visible);
            QString error; const bool ok=worker.reload(cfg,error);
            // First-frame publication includes import, upload and shader work.
            // GPU work stays serial, with one bounded frame mailbox per view.
            auto frame=worker.snapshot();
            if(worker.frameReady)worker.frameReady(std::move(frame));
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
            next["ovoxel_mode"].toString("surface")!=old["ovoxel_mode"].toString("surface");
        config=cfg;
        if(reload && statistics["engine"].toBool()) { pending=true; if(loadingChanged)loadingChanged(); }
        command([cfg](ModelWorker& worker){worker.settings(cfg);});
    }
    void fit() override { command([](ModelWorker& worker){worker.fit();}); }
    void resetView() override { command([](ModelWorker& worker){worker.resetView();}); }
    QJsonObject metrics() const override {
        auto result=statistics; result["loading"]=pending; result["render_worker_limit"]=1;
        result["command_pending"]=state->pendingCommands>0 || statistics["render_pending"].toBool(); return result;
    }
    bool loading() const override { return pending; }
    QImage snapshot() override { return image; }
    QJsonObject cameraState() const override { return statistics["camera"].toObject(); }
    QJsonObject cameraHome() const override { return home; }
    void setCameraState(const QJsonObject& camera) override { command([camera](ModelWorker& worker){worker.setCameraState(camera);}); }
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
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const bool light=config["settings"].toObject()["background"]=="light";
        painter.fillRect(rect(),light?QColor("#cccccc"):QColor("#13171d"));
        if(!image.isNull())painter.drawImage(rect(),image);
    }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event); const auto extent=size(); const auto scale=devicePixelRatioF();
        command([extent,scale](ModelWorker& worker){worker.viewport(extent,scale);});
    }
    void showEvent(QShowEvent* event) override { QWidget::showEvent(event); command([](ModelWorker& worker){worker.exposed(true);}); }
    void hideEvent(QHideEvent* event) override { QWidget::hideEvent(event); command([](ModelWorker& worker){worker.exposed(false);}); }
    void wheelEvent(QWheelEvent* event) override { const auto delta=event->angleDelta().y(); command([delta](ModelWorker& worker){worker.scroll(delta);}); event->accept(); }
    void mousePressEvent(QMouseEvent* event) override { last=event->position(); if(activated)activated(); setFocus(); }
    void mouseReleaseEvent(QMouseEvent* event) override {
        command([](ModelWorker& worker){if(worker.inspectionChanged)worker.inspectionChanged(worker.cameraState(),true);}); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        const auto delta=event->position()-last; last=event->position();
        if(event->buttons()==Qt::NoButton)return;
        const bool pan=event->buttons().testFlag(Qt::MiddleButton) || event->buttons().testFlag(Qt::RightButton) || event->modifiers().testFlag(Qt::ShiftModifier);
        command([delta,pan](ModelWorker& worker){worker.drag(delta,pan);});
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { fit(); }
private:
    template<class Action> void command(Action action) {
        post([action](ModelWorker& worker){action(worker); return std::pair{true,QString()};},state->revision,false);
    }
    template<class Action> void post(Action action,unsigned version,bool completion) {
        QPointer<ModelSurface> guard(this); const auto slot=state;
        ++slot->pendingCommands;
        QElapsedTimer queued; queued.start();
        const int priority=statistics["engine"].toBool()?(completion?1:2):0;
        renderWork([guard,slot,version,completion,action,queued] {
            const auto finished=qScopeGuard([slot] { --slot->pendingCommands; });
            if(slot->dead || version!=slot->revision)return;
            slot->activeRevision=version;
            if(completion)slot->queueMs=queued.nsecsElapsed()/1e6;
            if(!slot->worker) { if(completion)publish(guard,slot,{},std::pair{false,slot->failure}); return; }
            try {
                const auto result=action(*slot->worker);
                publish(guard,slot,{},completion?std::optional(result):std::nullopt);
            } catch(const std::exception& error) { publish(guard,slot,{},std::pair{false,QString::fromUtf8(error.what())}); }
        },priority,slot.get());
    }
    static void publish(QPointer<ModelSurface> guard,const std::shared_ptr<ModelState>& slot,QImage frame={},std::optional<std::pair<bool,QString>> completion={}) {
        if(slot->dead || slot->activeRevision!=slot->revision)return;
        QMutexLocker lock(&slot->mutex);
        if(slot->publishedRevision!=slot->activeRevision) {
            slot->frame={}; slot->completion.reset();
        }
        if(!frame.isNull())slot->frame=std::move(frame);
        if(slot->worker) {
            slot->metrics=slot->worker->metrics(); slot->home=slot->worker->cameraHome();
            auto timing=slot->metrics["load_timing"].toObject(); timing["queue_ms"]=slot->queueMs;
            slot->metrics["load_timing"]=timing;
        }
        if(completion)slot->completion=completion;
        slot->publishedRevision=slot->activeRevision;
        if(slot->notification)return;
        slot->notification=true;
        QMetaObject::invokeMethod(qApp,[guard,slot] {
            QImage frame; QJsonObject metrics,home; std::optional<std::pair<bool,QString>> completion; unsigned version;
            {
                QMutexLocker lock(&slot->mutex); frame=std::move(slot->frame); metrics=slot->metrics; home=slot->home;
                completion=std::move(slot->completion); slot->completion.reset(); version=slot->publishedRevision; slot->notification=false;
            }
            if(!guard || slot->dead || version!=slot->revision)return;
            guard->statistics=metrics; guard->home=home;
            if(!frame.isNull()) { guard->image=std::move(frame); guard->update(); }
            if(completion) {
                guard->pending=false;
                if(guard->loaded)guard->loaded(completion->first,completion->second);
            }
            if(guard->playbackChanged)guard->playbackChanged();
        },Qt::QueuedConnection);
    }
    std::shared_ptr<ModelState> state;
    LoadedCallback loaded;
    QJsonObject config,statistics,home;
    QImage image;
    QPointF last;
    bool pending=false;
};
}
ModelHandle createModel(QJsonObject config,QWidget* parent,LoadedCallback loaded,InspectionCallback changed) {
    auto* surface=new ModelSurface(std::move(config),parent,std::move(loaded),std::move(changed));
    return {surface,surface};
}
