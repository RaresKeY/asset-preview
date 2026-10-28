#include "thumbnail_queue.h"
#include <QCoreApplication>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRunnable>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <csignal>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
constexpr int ImageWorkers=2,BackgroundDelay=250;
QImage smallImage(const QString& path) {
    const QFileInfo file(path);
    if (!file.isFile() || file.size()>16*1024*1024) return {};
    QImageReader reader(path); reader.setAutoTransform(true);
    static const QSet<QByteArray> formats{"png","jpeg","webp","bmp","tiff"};
    if (!formats.contains(reader.format().toLower())) return {};
    const auto size=reader.size(); const auto format=reader.imageFormat();
    const int bytes=format==QImage::Format_Invalid?16:std::max(4,int(QImage::toPixelFormat(format).bitsPerPixel()+7)/8);
    if (!size.isValid() || qint64(size.width())*size.height()*bytes>32*1024*1024) return {};
    reader.setScaledSize(size.scaled(preview::PreviewCache::ThumbnailWidth,preview::PreviewCache::ThumbnailHeight,Qt::KeepAspectRatio));
    return reader.read().convertToFormat(QImage::Format_RGBA8888);
}
}
ThumbnailQueue::ThumbnailQueue(preview::PreviewCache* store,QString path,QObject* parent):QObject(parent),cache(store),root(std::move(path)) {
    pool.setMaxThreadCount(ImageWorkers); pool.setExpiryTimeout(1000); pool.setThreadPriority(QThread::LowPriority);
    dispatch.setSingleShot(true); deadline.setSingleShot(true);
    connect(&dispatch,&QTimer::timeout,this,[this] { debouncePending=false; work(); });
    connect(&deadline,&QTimer::timeout,this,[this] { timedOut=true; cancelRender(); });
    quiet.start();
}
ThumbnailQueue::~ThumbnailQueue() {
    dispatch.stop();
    for (auto& job:decodes) *job.cancelled=true;
    cancelRender();
    if (process) { process->disconnect(this); process->waitForFinished(1000); }
    pool.clear(); pool.waitForDone();
}
bool ThumbnailQueue::eligible(const QString& id) const { return enabled && byId.contains(id) && !excluded.contains(id); }
bool ThumbnailQueue::inFlight(const QString& key) const { return decodes.contains(key) || (process && renderKey==key); }
void ThumbnailQueue::setItems(const QList<Item>& next) {
    if (items==next) return;
    QMap<QString,Item> nextById;
    for (const auto& item:next) {
        nextById.insert(item.id,item);
        if (!byId.contains(item.id) || byId.value(item.id)!=item) {
            dirty.insert(item.id); hydrated.remove(item.id); keys.remove(item.id);
            for (auto& job:decodes) if(job.id==item.id)*job.cancelled=true;
            if(renderId==item.id)cancelRender();
        }
    }
    items=next; byId=std::move(nextById);
    const QSet<QString> retainedIds(byId.keyBegin(),byId.keyEnd());
    dirty.intersect(retainedIds); hydrated.intersect(retainedIds);
    for (auto it=keys.begin();it!=keys.end();) {
        if (!byId.contains(it.key())) { dirty.remove(it.key()); hydrated.remove(it.key()); it=keys.erase(it); }
        else ++it;
    }
    for (auto& job:decodes) if(!byId.contains(job.id))*job.cancelled=true;
    if (!renderId.isEmpty() && !byId.contains(renderId))cancelRender();
    QSet<QString> retained; for(const auto& key:keys)retained.insert(key);
    attempted.intersect(retained); schedule();
}
void ThumbnailQueue::demand(QStringList front,QStringList next,QSet<QString> omit,bool active,bool background) {
    const bool navigation=visible!=front || nearby!=next || excluded!=omit || enabled!=active;
    const bool changedDemand=navigation || warm!=background;
    if (!changedDemand) return;
    if(navigation)backgroundSuppressed=false;
    const auto oldVisible=visible;
    const auto oldNearby=nearby; const auto oldExcluded=excluded;
    visible=std::move(front); nearby=std::move(next); excluded=std::move(omit); enabled=active; warm=background;
    quiet.restart();
    // A key may have changed while its source was offscreen/unwatched.
    for(const auto& id:visible)if(!oldVisible.contains(id)) { dirty.insert(id); hydrated.remove(id); }
    for(const auto& id:nearby)if(!oldNearby.contains(id)) { dirty.insert(id); hydrated.remove(id); }
    for(const auto& id:oldExcluded)if(!excluded.contains(id)) { dirty.insert(id); hydrated.remove(id); }
    for(auto& job:decodes)
        if(!eligible(job.id) || (!visible.contains(job.id) && !warm)) *job.cancelled=true;
    if(process && (!eligible(renderId) || (renderForeground && !visible.contains(renderId)) || (!visible.contains(renderId) && !warm)))cancelRender();
    if (!enabled) {
        dispatch.stop();
        // Hidden RAM is cleared by the owner. A later show rehydrates disk.
        hydrated.clear(); dirty.clear(); keys.clear(); attempted.clear();
    } else schedule();
    notify();
}
void ThumbnailQueue::refresh(bool reset) {
    for (const auto& item:items)dirty.insert(item.id);
    hydrated.clear();
    if(reset) { backgroundSuppressed=true; attempted.clear(); for(auto& job:decodes)*job.cancelled=true; cancelRender(); }
    quiet.restart(); schedule();
}
void ThumbnailQueue::schedule(int delay,bool debounce) {
    if(!enabled) { dispatch.stop(); return; }
    // Only user/source debounces protect their quiet period. A background
    // deadline must never hold up a newly freed foreground worker slot.
    if(debounce || !dispatch.isActive() || (!debouncePending && delay<dispatch.remainingTime())) {
        debouncePending=debounce; dispatch.start(delay);
    }
}
void ThumbnailQueue::work() {
    if(!enabled || !cache->allowsPreload())return;
    // Visible images first, then visible models; nearby/remaining work follows
    // only after a finite quiet period. No queued worker backlog can outrank a
    // newly visible request. The two decode slots are independent of F3D.
    QStringList order;
    auto append=[&](const QStringList& ids) {
        for(bool images:{true,false})for(const auto& id:ids)
            if(eligible(id) && (byId.value(id).kind=="image")==images && !order.contains(id))order.append(id);
    };
    append(visible);
    if(warm && !backgroundSuppressed) { append(nearby); QStringList rest; for(const auto& item:items)rest.append(item.id); append(rest); }
    QElapsedTimer slice; slice.start(); int prepared=0;
    for(const auto& id:order) {
        const auto item=byId.value(id); const bool front=visible.contains(id);
        if(!front && quiet.elapsed()<BackgroundDelay) { schedule(BackgroundDelay-int(quiet.elapsed()),false); break; }
        if(!front && item.kind!="image" && item.kind!="model" && item.kind!="material")continue;
        if(dirty.contains(id) || !keys.contains(id) || !hydrated.contains(id)) {
            if(prepared>=4 || (prepared && slice.elapsed()>=8)) { schedule(1,false); break; }
            ++prepared;
            const auto old=keys.value(id),key=keyFor?keyFor(id):QString();
            keys[id]=key; dirty.remove(id); hydrated.insert(id);
            if(key!=old) {
                for(auto& job:decodes)if(job.id==id && job.key!=key)*job.cancelled=true;
                if(renderId==id && renderKey!=key)cancelRender();
            }
            // Hydrate before checking attempts: the RAM pool can have evicted a
            // successful output while a full image was retained elsewhere.
            if(!key.isEmpty() && (front || cache->peekThumbnail(key).isNull()))cache->thumbnail(key);
            if(changed)changed(id,key);
            if(front && (item.kind=="model" || item.kind=="material") && sourcesChanged)sourcesChanged();
        }
        const auto key=keys.value(id);
        if(key.isEmpty() || !cache->peekThumbnail(key).isNull() || inFlight(key) || attempted.contains(key))continue;
        if(item.kind=="image") {
            // Leave the second slot available for foreground arrivals during
            // warming. Decode cancellation is cooperative, never a GUI wait.
            if(decodes.size()>=(front?ImageWorkers:1))continue;
            attempted.insert(key); decode(item,key,front);
        } else if(item.kind=="model" || item.kind=="material") {
            if(process) {
                if(front && !renderForeground && !cancelled)cancelRender();
                continue;
            }
            attempted.insert(key); render(item,key,front);
        } else attempted.insert(key);
    }
    // Superseded source identities must not accumulate across save cycles.
    QSet<QString> retained; for(const auto& key:keys)retained.insert(key);
    attempted.intersect(retained);
}
void ThumbnailQueue::notify() { if(activityChanged)activityChanged(); }
void ThumbnailQueue::publish(const QString& key,const QImage& image) {
    cache->putThumbnail(key,image);
    for(auto it=keys.cbegin();it!=keys.cend();++it)if(it.value()==key && changed)changed(it.key(),key);
}
void ThumbnailQueue::decode(const Item& item,const QString& key,bool front) {
    const auto token=std::make_shared<std::atomic_bool>(false);
    decodes.insert(key,{item.id,key,front,token}); peakImages=std::max(peakImages,int(decodes.size())); notify();
    const auto source=preview::sourceCacheKey("thumbnail-source-v1",item.path);
    QPointer<ThumbnailQueue> guard(this);
    pool.start(QRunnable::create([guard,item,key,source,token,front] {
        ::setpriority(PRIO_PROCESS,0,19);
#ifdef SYS_ioprio_set
        ::syscall(SYS_ioprio_set,1,0,3<<13);
#endif
        const int nice=::getpriority(PRIO_PROCESS,0);
        QElapsedTimer timer; timer.start();
        QImage image;
        if(!*token)image=smallImage(item.path);
        if(*token || source!=preview::sourceCacheKey("thumbnail-source-v1",item.path))image={};
        const auto elapsed=timer.nsecsElapsed()/1000;
        if(!guard)return;
        QMetaObject::invokeMethod(guard,[guard,item,key,token,front,elapsed,nice,image=std::move(image)]() mutable {
            if(!guard)return;
            guard->decodes.remove(key);
            const bool valid=!*token && guard->eligible(item.id) && (guard->visible.contains(item.id) || guard->warm) &&
                guard->keyFor && guard->keyFor(item.id)==key && !image.isNull();
            if(*token || (!valid && !image.isNull()))guard->attempted.remove(key);
            guard->cache->observe("thumbnail",valid?"generated":image.isNull()?"skipped":"discarded",key,image.sizeInBytes(),
                valid?(front?"visible":"background_image"):*token?"cancelled":"bounded_or_replaced",elapsed,{{"nice",nice}});
            if(valid) { guard->publish(key,image); ++guard->decoded; }
            guard->notify(); guard->schedule(valid?1:25,!valid);
        },Qt::QueuedConnection);
    }));
}
void ThumbnailQueue::render(const Item& item,const QString& key,bool front) {
    const auto config=configFor?configFor(item.id):QJsonObject{};
    const QFileInfo source(item.path); const auto request=QJsonDocument(config).toJson(QJsonDocument::Compact);
    if(config.isEmpty() || root.isEmpty() || !source.isFile() || source.size()>256*1024*1024 || request.size()>64*1024)return;
    directory=std::make_unique<QTemporaryDir>(root+"/job-XXXXXX");
    if(!directory->isValid()) { directory.reset(); return; }
    QFile input(directory->filePath("request.json"));
    if(!input.open(QIODevice::WriteOnly) || input.write(request)!=request.size()) { directory.reset(); return; }
    input.close();
    cancelled=timedOut=false; renderId=item.id; renderKey=key; renderForeground=front;
    auto* child=new QProcess(this); process=child;
    child->setChildProcessModifier([owner=::getpid()] {
        if(::setsid()<0 || ::prctl(PR_SET_PDEATHSIG,SIGKILL)<0 || ::getppid()!=owner)_exit(126);
    });
    auto environment=QProcessEnvironment::systemEnvironment(); environment.insert("TMPDIR",directory->path());
    child->setProcessEnvironment(environment); child->setStandardOutputFile(QProcess::nullDevice()); child->setStandardErrorFile(QProcess::nullDevice());
    auto elapsed=std::make_shared<QElapsedTimer>(); elapsed->start();
    auto finish=[this,child,item,key,front,elapsed](bool success) {
        if(process!=child)return;
        deadline.stop(); QFile receipt(directory->filePath("result.json")); QJsonObject details;
        if(receipt.open(QIODevice::ReadOnly) && receipt.size()<=4096)details=QJsonDocument::fromJson(receipt.readAll()).object();
        QImage image; const auto path=directory->filePath("thumbnail.png");
        if(success && QFileInfo(path).size()<=256*1024)image=smallImage(path);
        const bool valid=!cancelled && eligible(item.id) && (visible.contains(item.id) || warm) && keyFor && keyFor(item.id)==key && !image.isNull();
        if((cancelled && !timedOut) || (!valid && !image.isNull()))attempted.remove(key);
        if(!details["renderer"].toString().isEmpty())lastRenderer=details["renderer"].toString();
        lastError=details["error"].toString();
        if(!success && !cancelled && lastError.isEmpty())lastError="Thumbnail worker failed";
        if(timedOut)lastError="Thumbnail timed out";
        cache->observe("thumbnail",valid?"generated":image.isNull()?"skipped":"discarded",key,image.sizeInBytes(),
            valid?(front?"visible_model":"background_model"):timedOut?"timeout":cancelled?"cancelled":"bounded_or_failed",elapsed->nsecsElapsed()/1000,details);
        if(valid) { publish(key,image); ++rendered; }
        process.clear(); directory.reset(); renderId.clear(); renderKey.clear(); child->deleteLater();
        notify(); if(sourcesChanged)sourcesChanged(); schedule(valid?1:25,!valid);
    };
    connect(child,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[finish](int code,QProcess::ExitStatus status) { finish(code==0 && status==QProcess::NormalExit); });
    connect(child,&QProcess::errorOccurred,this,[finish](QProcess::ProcessError error) { if(error==QProcess::FailedToStart)finish(false); });
    deadline.start(30000); child->start(QCoreApplication::applicationFilePath(),{"--thumbnail-job",directory->path()}); notify(); if(sourcesChanged)sourcesChanged();
}
void ThumbnailQueue::cancelRender() {
    if(!process)return;
    cancelled=true; deadline.stop();
    const auto pid=process->processId(); if(pid>0)::kill(-pid,SIGKILL);
    process->kill();
}
QSet<QString> ThumbnailQueue::loadingIds() const {
    QSet<QString> ids;
    for(const auto& job:decodes)if(!*job.cancelled)ids.insert(job.id);
    if(process && !cancelled)ids.insert(renderId);
    return ids;
}
QStringList ThumbnailQueue::workingIds() const {
    QStringList ids; for(const auto& job:decodes)ids.append(job.id);
    if(process)ids.append(renderId);
    return ids;
}
QJsonObject ThumbnailQueue::metrics() const {
    QJsonArray images; for(const auto& job:decodes)images.append(QJsonObject{{"id",job.id},{"foreground",job.foreground},{"cancelled",job.cancelled->load()}});
    bool ready=true;
    for(const auto& id:visible)if(eligible(id)) {
        const auto key=keys.value(id);
        if(dirty.contains(id) || !hydrated.contains(id) || (!key.isEmpty() && cache->peekThumbnail(key).isNull() && (!attempted.contains(key) || inFlight(key)))) { ready=false; break; }
    }
    return {{"worker_active",!decodes.isEmpty() || bool(process) || dispatch.isActive()},{"worker_limit",3},
        {"visible_ready",ready},{"foreground_active",enabled && !ready},
        {"image_worker_limit",ImageWorkers},{"image_workers",images},{"image_peak",peakImages},{"render_worker_limit",1},
        {"worker_id",renderId},{"worker_pid",process?double(process->processId()):0},{"worker_nice",19},
        {"rendered",rendered},{"decoded",decoded},{"renderer",lastRenderer},{"render_error",lastError},
        {"warming",warm && !backgroundSuppressed},{"enabled",enabled},{"targets",items.size()},{"thumbnail_width",256},{"thumbnail_height",160}};
}
