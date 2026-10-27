#include "preview_preload.h"
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QPointer>
#include <QRunnable>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <array>

namespace {
constexpr qint64 ReadLimit=32*preview::PreviewCache::MiB;
QString fingerprint(const QStringList& paths) {
    QByteArray identity;
    for(const auto& path:paths)identity+=path.toUtf8()+'\0'+preview::sourceCacheKey("preload-dependency-v1",path).toUtf8()+'\0';
    return QString::fromLatin1(QCryptographicHash::hash(identity,QCryptographicHash::Sha256).toHex());
}
preview::CachedPayload decode(const QString& path,const std::atomic_bool& cancelled) {
    if (cancelled || QFileInfo(path).size()>16*preview::PreviewCache::MiB) return {};
    QImageReader reader(path); reader.setAutoTransform(true);
    static const QSet<QByteArray> formats{"png","jpeg","webp","bmp","tiff","exr","hdr"};
    if (!formats.contains(reader.format().toLower())) return {};
    const auto size=reader.size(); const auto format=reader.imageFormat();
    const int pixelBytes=format==QImage::Format_Invalid?16:std::max(4,int(QImage::toPixelFormat(format).bitsPerPixel()+7)/8);
    if (!size.isValid() || qint64(size.width())*size.height()*pixelBytes>preview::PreviewCache::ItemLimit) return {};
    if (size.width()>4096 || size.height()>4096) reader.setScaledSize(size.scaled(4096,4096,Qt::KeepAspectRatio));
    if (cancelled) return {};
    auto image=reader.read();
    if (cancelled || image.sizeInBytes()>preview::PreviewCache::ItemLimit) return {};
    return {std::move(image),size,{}};
}
}

PreviewPreloader::PreviewPreloader(preview::PreviewCache* store):cache(store) {
    QImageReader::setAllocationLimit(64);
    worker.setMaxThreadCount(1); worker.setExpiryTimeout(1000);
    worker.setThreadPriority(QThread::LowPriority);
}
PreviewPreloader::~PreviewPreloader() {
    if (cancelled) *cancelled=true;
    worker.clear(); worker.waitForDone();
}
void PreviewPreloader::target(const QString& nextId,const QString& nextKind,const QString& nextPath,const QStringList& dependencies) {
    const auto nextIdentity=nextId.isEmpty()?QString():preview::sourceCacheKey(nextKind=="image"?"image-v1-4096":"preload-file-v1",nextPath);
    QStringList nextPaths;
    if(!nextId.isEmpty())nextPaths.append(nextPath);
    qsizetype pathBytes=nextPath.toUtf8().size();
    if(nextKind!="image")for(const auto& dependency:dependencies) {
        if(nextPaths.contains(dependency))continue;
        pathBytes+=dependency.toUtf8().size();
        if(nextPaths.size()>=64 || pathBytes>16*1024)break;
        nextPaths.append(dependency);
    }
    const auto nextDependencyIdentity=fingerprint(nextPaths);
    if (id==nextId && kind==nextKind && path==nextPath && identity==nextIdentity && dependencyIdentity==nextDependencyIdentity) return;
    if (cancelled) *cancelled=true;
    id=nextId; kind=nextKind; path=nextPath; identity=nextIdentity;
    paths=nextPaths; dependencyIdentity=nextDependencyIdentity; lastBytes=lastUs=0;
    state=id.isEmpty()?"idle":identity.isEmpty()?"missing":"pending";
    start();
}
void PreviewPreloader::start() {
    if (busy || state!="pending") return;
    if (kind=="image" && cache->hasImage(identity)) { state="cached"; return; }
    if (!cache->allowsPreload()) { state="memory_pressure"; return; }
    busy=true; state="loading";
    cancelled=std::make_shared<std::atomic_bool>(false);
    const auto token=cancelled; const auto source=path, key=identity, type=kind, dependenciesKey=dependencyIdentity;
    const auto sources=paths;
    cache->identify(key,type,source);
    QPointer<PreviewPreloader> guard(this);
    worker.start(QRunnable::create([guard,token,source,key,type,sources,dependenciesKey] {
        QElapsedTimer timer; timer.start();
        preview::CachedPayload payload; qint64 bytes=0;
        // Only regular local files: never block opening a FIFO/device.
        if (QFileInfo(source).isFile()) {
            if (type=="image") { payload=decode(source,*token); bytes=payload.bytes(); }
            else {
                for(const auto& path:sources) {
                if(*token || bytes>=ReadLimit)break;
                if(!QFileInfo(path).isFile())continue;
                QFile file(path);
                if (!*token && file.open(QIODevice::ReadOnly)) {
                    std::array<char,64*1024> buffer;
                    while (!*token && bytes<ReadLimit) {
                        const auto count=file.read(buffer.data(),std::min(qint64(buffer.size()),ReadLimit-bytes));
                        if (count<=0) break;
                        bytes+=count;
                    }
                }
                }
            }
        }
        const auto elapsed=timer.nsecsElapsed()/1000;
        if (!guard) return;
        QMetaObject::invokeMethod(guard,[guard,token,source,key,type,bytes,elapsed,sources,dependenciesKey,payload=std::move(payload)]() mutable {
            if (!guard) return;
            guard->busy=false;
            const bool current=!*token && key==preview::sourceCacheKey(type=="image"?"image-v1-4096":"preload-file-v1",source) && dependenciesKey==fingerprint(sources);
            if (current) {
                guard->lastBytes=bytes; guard->lastUs=elapsed;
                if (!payload.image.isNull() && !guard->cache->hasImage(key)) guard->cache->put(key,std::move(payload));
                guard->state=type=="image"?(guard->cache->hasImage(key)?"cached":"skipped"):(bytes?"warmed":"skipped");
            } else if (!*token) guard->state="stale";
            guard->cache->observe("preload",current?"complete":"discarded",key,bytes,
                current?guard->state:"hidden_or_replaced",elapsed);
            guard->start();
        },Qt::QueuedConnection);
    }));
}
QJsonObject PreviewPreloader::metrics() const {
    return {{"target",id},{"state",state},{"worker_active",busy},{"worker_limit",1},
        {"files",paths.size()},{"bytes",double(lastBytes)},{"elapsed_ms",lastUs/1000.0},{"low_priority",true},
        {"read_limit",double(ReadLimit)},{"image_limit",double(preview::PreviewCache::ItemLimit)}};
}
