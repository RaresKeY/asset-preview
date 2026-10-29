#include "preview_cache.h"
#include "cache_diagnostics.h"
#include <QBuffer>
#include <QDataStream>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSet>
#include <QHash>
#include <algorithm>
#include <cmath>
#include <list>
#include <sys/stat.h>
#include <unistd.h>

namespace preview {
namespace {
bool regular(const QString& path) {
    struct stat s{};
    return ::lstat(QFile::encodeName(path).constData(),&s)==0 && S_ISREG(s.st_mode) && s.st_uid==::geteuid() && s.st_nlink==1;
}
QByteArray readFile(const QString& path,qint64 maximum) {
    if (!regular(path)) return {};
    QFile f(path); if (!f.open(QIODevice::ReadOnly) || f.size()>maximum) return {};
    return f.read(maximum+1);
}
bool publish(const QString& path,const QByteArray& bytes) {
    if (QFileInfo(path).isSymLink() || (QFileInfo::exists(path) && !regular(path))) return false;
    QSaveFile f(path); f.setDirectWriteFallback(false);
    return f.open(QIODevice::WriteOnly) && f.setPermissions(QFile::ReadOwner|QFile::WriteOwner) && f.write(bytes)==bytes.size() && f.commit();
}
bool lowMemory() {
#if defined(Q_OS_LINUX)
    QFile f("/proc/meminfo"); if (!f.open(QIODevice::ReadOnly)) return false;
    qint64 available=-1,total=0;
    for (const auto& line:f.read(8192).split('\n')) {
        if (line.startsWith("MemAvailable:")) available=line.simplified().split(' ').value(1).toLongLong();
        if (line.startsWith("MemTotal:")) total=line.simplified().split(' ').value(1).toLongLong();
    }
    return available>=0 && available<std::max(qint64(256*1024),total/20);
#else
    return false;
#endif
}
}
// Reserve the small-object partition before accepting disk entries, including
// migration from an older texture cache whose quota was the full 256 MiB.
bool reserveTexturePartition(CacheDiagnostics* diagnostics=nullptr) {
    QElapsedTimer timer; timer.start();
    const auto root=QDir::cleanPath(qEnvironmentVariable("ASSET_PREVIEW_TEXTURE_CACHE",QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/texture-previews-v1"));
    struct stat s{};
    if (::lstat(QFile::encodeName(root).constData(),&s)!=0) return true;
    if (!S_ISDIR(s.st_mode) || s.st_uid!=::geteuid() || (s.st_mode&0077) ||
        readFile(QDir(root).filePath(".asset-preview-textures"),128)!="Asset Preview texture derivatives v1\n") return false;
    QLockFile lock(QDir(root).filePath("cache.lock")); lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) return false;
    QList<QFileInfo> files; qint64 total=0; int scanned=0;
    QDirIterator scan(root,QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot);
    const QRegularExpression owned("^v1-[0-9a-f]{64}\\.(glb|json)$");
    while (scan.hasNext()) {
        scan.next(); if (++scanned>8192) return false;
        const auto f=scan.fileInfo(); if (!owned.match(f.fileName()).hasMatch()) continue;
        if (!regular(f.filePath())) return false;
        files.append(f); total+=f.size();
    }
    std::sort(files.begin(),files.end(),[](const QFileInfo& a,const QFileInfo& b) { return a.lastModified()<b.lastModified(); });
    int removed=0;
    for (const auto& f:files) {
        if (total<=224*PreviewCache::MiB) break;
        if (!QFile::remove(f.filePath())) return false;
        total-=f.size();
        ++removed;
        if (diagnostics) diagnostics->record("glb_disk","prune",f.completeBaseName().mid(3),f.size(),"partition_reservation");
    }
    if (diagnostics) diagnostics->record("glb_disk","prune_scan",{},total,"partition_reservation",timer.nsecsElapsed()/1000,
        {{"files",files.size()},{"removed",removed}});
    return total<=224*PreviewCache::MiB;
}
struct PreviewCache::Impl {
    CacheDiagnostics diagnostics;
    struct Item { QString key; CachedPayload payload; bool thumbnail=false; };
    std::list<Item> items;
    QHash<QString,std::list<Item>::iterator> index;
    qint64 bytes=0,thumbnailBytes=0,diskBytes=0;
    int diskEntries=0;
    quint64 hits=0,misses=0,diskHits=0,evictions=0,rejected=0;
    QString root;
    QSet<QString> absent;
    bool prepared=false,scanned=false;
    bool prepare() {
        if (prepared) return true;
        root=QDir::cleanPath(qEnvironmentVariable("ASSET_PREVIEW_SMALL_CACHE",QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/small-previews-v1"));
        if (!QFileInfo(root).isAbsolute()) return false;
        struct stat s{};
        if (::lstat(QFile::encodeName(root).constData(),&s)!=0) {
            if (!QDir().mkpath(QFileInfo(root).absolutePath()) || ::mkdir(QFile::encodeName(root).constData(),0700)!=0) return false;
            if (::lstat(QFile::encodeName(root).constData(),&s)!=0) return false;
        }
        if (!S_ISDIR(s.st_mode) || s.st_uid!=::geteuid() || (s.st_mode&0077)) return false;
        const auto marker=QDir(root).filePath(".asset-preview-small");
        const QByteArray contents="Asset Preview small derivatives v1\n";
        if (readFile(marker,128)!=contents && (!QDir(root).isEmpty() || !publish(marker,contents))) return false;
        if (!reserveTexturePartition(&diagnostics)) return false;
        prepared=true; return true;
    }
    QString path(const QString& key) const { return QDir(root).filePath("v1-"+key+".bin"); }
    bool prune(qint64 reserve=0,const QString& keep={},bool all=false) {
        QElapsedTimer timer; timer.start();
        struct Disk { QString path; qint64 bytes; QDateTime used; };
        QList<Disk> files; qint64 total=0;
        const QRegularExpression owned("^v1-[0-9a-f]{64}\\.bin$");
        QDirIterator scan(root,QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot); int scanned=0;
        while (scan.hasNext()) {
            scan.next(); if (++scanned>4096) return false;
            const auto f=scan.fileInfo(); if (!owned.match(f.fileName()).hasMatch()) continue;
            if (!regular(f.filePath())) return false;
            files.append({f.filePath(),f.size(),f.lastModified()}); total+=f.size();
        }
        std::sort(files.begin(),files.end(),[](const Disk& a,const Disk& b) { return a.used<b.used; });
        int count=files.size(),removed=0; const auto expiry=QDateTime::currentDateTimeUtc().addDays(-30);
        for (const auto& f:files) {
            const bool expired=f.used<expiry;
            if (!all && !expired && total+reserve<=DiskLimit && count+(reserve?1:0)<=DiskEntries) break;
            if (!all && f.path==path(keep)) continue;
            if (!QFile::remove(f.path)) return false;
            const auto reason=all?QString("manual"):expired?QString("expired"):total+reserve>DiskLimit?QString("disk_bytes"):QString("disk_entries");
            diagnostics.record("small_disk","prune",QFileInfo(f.path).completeBaseName().mid(3),f.bytes,reason);
            total-=f.bytes; --count;
            ++removed;
        }
        diskBytes=total; diskEntries=count;
        diagnostics.record("small_disk","prune_scan",{},total,all?"manual":"admission",timer.nsecsElapsed()/1000,
            {{"files",files.size()},{"removed",removed}});
        return total+reserve<=DiskLimit && count+(reserve?1:0)<=DiskEntries;
    }
    QByteArray read(const QString& key) {
        if (key.isEmpty() || absent.contains(key)) return {};
        if (!prepare()) { diagnostics.record("small_disk","bypass",key,0,"cache_unavailable"); return {}; }
        QElapsedTimer timer; timer.start();
        QLockFile lock(QDir(root).filePath("cache.lock")); lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) { diagnostics.record("small_disk","bypass",key,0,"cache_busy"); return {}; }
        if (!scanned) { if (!prune()) return {}; scanned=true; }
        auto data=readFile(path(key),DiskItemLimit);
        if (data.size()<36 || data.left(4)!="APC1" || data.mid(4,32)!=QCryptographicHash::hash(data.mid(36),QCryptographicHash::Sha256)) {
            if (absent.size()>=1024) absent.clear();
            absent.insert(key);
            diagnostics.record("small_disk","miss",key,0,data.isEmpty()?"absent_or_unreadable":"corrupt",timer.nsecsElapsed()/1000);
            return {};
        }
        QFile f(path(key)); if (f.open(QIODevice::ReadOnly)) f.setFileTime(QDateTime::currentDateTimeUtc(),QFileDevice::FileModificationTime);
        ++diskHits;
        diagnostics.record("small_disk","hit",key,data.size(),{},timer.nsecsElapsed()/1000);
        return data.mid(36);
    }
    void write(const QString& key,const QByteArray& payload) {
        if (key.isEmpty() || payload.isEmpty()) return;
        if (payload.size()+36>DiskItemLimit) { diagnostics.record("small_disk","reject",key,payload.size()+36,"item_limit"); return; }
        if (!prepare()) { diagnostics.record("small_disk","bypass",key,payload.size()+36,"cache_unavailable"); return; }
        QElapsedTimer timer; timer.start();
        QLockFile lock(QDir(root).filePath("cache.lock")); lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) { diagnostics.record("small_disk","bypass",key,payload.size()+36,"cache_busy"); return; }
        absent.remove(key);
        // Existing valid files need only a last-use touch, avoiding repeated writes.
        const auto data="APC1"+QCryptographicHash::hash(payload,QCryptographicHash::Sha256)+payload;
        if (readFile(path(key),DiskItemLimit)==data) {
            QFile f(path(key)); if (f.open(QIODevice::ReadOnly)) f.setFileTime(QDateTime::currentDateTimeUtc(),QFileDevice::FileModificationTime);
            diagnostics.record("small_disk","write_unchanged",key,data.size(),"identical_bytes",timer.nsecsElapsed()/1000);
            prune(0,key); return;
        }
        const auto previous=regular(path(key))?QFileInfo(path(key)).size():0;
        if (prune(data.size(),key) && publish(path(key),data)) {
            diskBytes+=data.size()-previous; if (!previous) ++diskEntries;
            diagnostics.record("small_disk","write",key,data.size(),previous?"replace":"new",timer.nsecsElapsed()/1000);
        }
    }
    auto find(const QString& key) {
        const auto it=index.constFind(key);
        return it!=index.cend()?it.value():items.end();
    }
    void erase(std::list<Item>::iterator i) {
        const auto n=i->payload.bytes(); bytes-=n; if (i->thumbnail) thumbnailBytes-=n;
        index.remove(i->key);
        items.erase(i);
    }
    void insert(QString key,CachedPayload payload,bool thumb) {
        const auto size=payload.bytes();
        if (key.isEmpty() || size<=0 || size>(thumb?ThumbnailLimit:ItemLimit)) {
            ++rejected; diagnostics.record("ram","reject",key,size,"item_limit"); return;
        }
        auto old=find(key);
        if (old!=items.end()) { diagnostics.record("ram","replace",key,old->payload.bytes(),"same_key"); erase(old); }
        while (!items.empty() && (bytes+size>RamLimit || items.size()>=EntryLimit || (thumb && thumbnailBytes+size>ThumbnailRamLimit))) {
            auto victim=items.begin();
            if(thumb && (bytes+size>RamLimit || items.size()>=EntryLimit)) {
                const auto payload=std::find_if(items.begin(),items.end(),[](const Item& i) { return !i.thumbnail; });
                if(payload!=items.end())victim=payload;
            }
            if (thumb && thumbnailBytes+size>ThumbnailRamLimit)
                victim=std::find_if(items.begin(),items.end(),[](const Item& i) { return i.thumbnail; });
            if (victim==items.end()) break;
            const auto reason=thumb && thumbnailBytes+size>ThumbnailRamLimit?QString("thumbnail_bytes"):bytes+size>RamLimit?QString("ram_bytes"):QString("ram_entries");
            const auto removedKey=victim->key; const auto removedBytes=victim->payload.bytes();
            QElapsedTimer freeTimer; freeTimer.start(); erase(victim); ++evictions;
            diagnostics.record("ram","evict",removedKey,removedBytes,reason,freeTimer.nsecsElapsed()/1000);
        }
        bytes+=size; if (thumb) thumbnailBytes+=size;
        items.push_back({std::move(key),std::move(payload),thumb});
        auto inserted=std::prev(items.end());
        index.insert(inserted->key,inserted);
        diagnostics.record("ram","admit",inserted->key,size,thumb?"thumbnail":"view_unload");
    }
};
PreviewCache::PreviewCache():d(std::make_unique<Impl>()) {}
PreviewCache::~PreviewCache()=default;
bool PreviewCache::hasImage(const QString& key) const {
    const auto i=d->find(key);
    return i!=d->items.end() && !i->thumbnail && !i->payload.image.isNull();
}
bool PreviewCache::allowsPreload() const { return !lowMemory(); }
CachedPayload PreviewCache::take(const QString& key,bool waveform) {
    trimForPressure();
    auto i=d->find(key);
    if (i!=d->items.end() && !i->thumbnail) {
        d->diagnostics.record("ram","take",key,i->payload.bytes(),"visible_view");
        ++d->hits; auto value=std::move(i->payload);
        // Account before erase: the payload has already moved.
        d->bytes-=value.bytes(); d->index.remove(i->key); d->items.erase(i); return value;
    }
    ++d->misses;
    d->diagnostics.record("ram","miss",key,0,"visible_view");
    if (!waveform) return {};
    const auto bytes=d->read(key);
    if (bytes.size()!=4+int(sizeof(float))*AudioWaveform::binCount*4+sizeof(double)+sizeof(qint32)+sizeof(float) || !bytes.startsWith("WAV1")) return {};
    QDataStream stream(bytes.mid(4)); stream.setByteOrder(QDataStream::LittleEndian); stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    auto wave=std::make_shared<AudioWaveform>();
    for (auto& bin:wave->bins) for (auto& v:bin) { stream>>v; if (!std::isfinite(v) || v<-1 || v>1) return {}; }
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision); stream>>wave->duration;
    qint32 channels=0; stream>>channels; wave->channels=channels;
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision); stream>>wave->peak;
    if (stream.status()!=QDataStream::Ok || !std::isfinite(wave->duration) || wave->duration<=0 || wave->duration>21600 ||
        channels<1 || channels>2 || !std::isfinite(wave->peak) || wave->peak<0 || wave->peak>1) return {};
    return {{},{},std::move(wave)};
}
void PreviewCache::put(const QString& key,CachedPayload payload) {
    trimForPressure();
    if (lowMemory()) { d->diagnostics.record("ram","reject",key,payload.bytes(),"memory_pressure"); return; }
    if (payload.waveform && !key.isEmpty()) {
        QByteArray data="WAV1";
        QDataStream stream(&data,QIODevice::Append); stream.setByteOrder(QDataStream::LittleEndian); stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
        for (const auto& bin:payload.waveform->bins) for (float v:bin) stream<<v;
        stream.setFloatingPointPrecision(QDataStream::DoublePrecision); stream<<payload.waveform->duration<<qint32(payload.waveform->channels);
        stream.setFloatingPointPrecision(QDataStream::SinglePrecision); stream<<payload.waveform->peak;
        d->write(key,data);
    }
    d->insert(key,std::move(payload),false);
}
QImage PreviewCache::thumbnail(const QString& key) {
    auto i=d->find(key);
    if (i!=d->items.end() && i->thumbnail) {
        d->diagnostics.record("ram","hit",key,i->payload.bytes(),"thumbnail");
        ++d->hits; d->items.splice(d->items.end(),d->items,i); return d->items.back().payload.image;
    }
    ++d->misses;
    d->diagnostics.record("ram","miss",key,0,"thumbnail");
    const auto data=d->read(key); if (data.size()<12 || !data.startsWith("IMG1")) return {};
    QDataStream stream(data.mid(4,8)); qint32 w=0,h=0; stream>>w>>h;
    if (w<1 || h<1 || w>256 || h>160 || data.size()!=12+w*h*4) return {};
    auto image=QImage(reinterpret_cast<const uchar*>(data.constData()+12),w,h,w*4,QImage::Format_RGBA8888).copy();
    if(w>ThumbnailWidth || h>ThumbnailHeight)image=image.scaled(ThumbnailWidth,ThumbnailHeight,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    if (!lowMemory()) d->insert(key,{image,{},{}},true);
    else d->diagnostics.record("ram","reject",key,image.sizeInBytes(),"memory_pressure");
    return image;
}
QImage PreviewCache::peekThumbnail(const QString& key) const {
    const auto i=d->find(key);
    return i!=d->items.end() && i->thumbnail?i->payload.image:QImage{};
}
void PreviewCache::putThumbnail(const QString& key,QImage image) {
    if (key.isEmpty() || image.isNull()) return;
    if (image.size()!=image.size().scaled(ThumbnailWidth,ThumbnailHeight,Qt::KeepAspectRatio))
        image=image.scaled(ThumbnailWidth,ThumbnailHeight,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    if (image.format()!=QImage::Format_RGBA8888)
        image=image.convertToFormat(QImage::Format_RGBA8888);
    QByteArray data="IMG1"; QDataStream stream(&data,QIODevice::Append); stream<<qint32(image.width())<<qint32(image.height());
    data.append(reinterpret_cast<const char*>(image.constBits()),image.sizeInBytes());
    d->write(key,data); trimForPressure();
    if (!lowMemory()) d->insert(key,{std::move(image),{},{}},true);
    else d->diagnostics.record("ram","reject",key,image.sizeInBytes(),"memory_pressure");
}
void PreviewCache::clear(bool disk,const QString& reason) {
    QElapsedTimer timer; timer.start();
    const auto freed=d->bytes; const int count=d->items.size();
    for (const auto& item:d->items) d->diagnostics.record("ram","discard",item.key,item.payload.bytes(),reason);
    d->items.clear(); d->index.clear(); d->absent.clear(); d->bytes=0; d->thumbnailBytes=0; d->scanned=false;
    if (count) d->diagnostics.record("ram","clear",{},freed,reason,timer.nsecsElapsed()/1000,{{"items",count}});
    if (disk && d->prepare()) {
        QLockFile lock(QDir(d->root).filePath("cache.lock")); lock.setStaleLockTime(0);
        if (lock.tryLock(0)) d->prune(0,{},true);
    }
}
void PreviewCache::trimForPressure() { if (lowMemory()) clear(false,"memory_pressure"); }
void PreviewCache::identify(const QString& key,const QString& kind,const QString& path) { d->diagnostics.identify(key,kind,path); }
void PreviewCache::observe(const QString& tier,const QString& action,const QString& key,qint64 bytes,const QString& reason,qint64 us,const QJsonObject& details) {
    d->diagnostics.record(tier,action,key,bytes,reason,us,details);
}
void PreviewCache::setDiagnosticsDirectory(const QString& directory) { d->diagnostics.setDirectory(directory); }
QJsonObject PreviewCache::events(int limit,quint64 after) const { return d->diagnostics.events(limit,after); }
QJsonObject PreviewCache::metrics() const {
    return {{"bytes",double(d->bytes)},{"item_limit",double(ItemLimit)},{"limit",double(RamLimit)},
        {"entries",int(d->items.size())},{"entry_limit",EntryLimit},{"thumbnail_bytes",double(d->thumbnailBytes)},
        {"thumbnail_limit",double(ThumbnailLimit)},{"thumbnail_ram_limit",double(ThumbnailRamLimit)},
        {"hits",double(d->hits)},{"misses",double(d->misses)},{"evictions",double(d->evictions)},{"rejected",double(d->rejected)},
        {"disk_hits",double(d->diskHits)},{"disk_bytes",double(d->diskBytes)},{"disk_entries",d->diskEntries},
        {"disk_limit",double(DiskLimit)},{"disk_item_limit",double(DiskItemLimit)},{"disk_entry_limit",DiskEntries},
        {"diagnostics",d->diagnostics.summary()}};
}
}
