#include "cache_diagnostics.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <algorithm>
#include <deque>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace preview {
namespace {
const QByteArray Header="{\"format\":\"asset-preview-cache-events\",\"version\":1}\n";
bool ownedLog(int fd) {
    struct stat s{}; char header[128]{};
    return fd>=0 && ::fstat(fd,&s)==0 && S_ISREG(s.st_mode) && s.st_uid==::geteuid() &&
        s.st_nlink==1 && !(s.st_mode&0077) && s.st_size<=CacheDiagnostics::LogLimit &&
        ::pread(fd,header,Header.size(),0)==Header.size() && QByteArray(header,Header.size())==Header;
}
}
struct CacheDiagnostics::Impl : QObject {
    struct Item {
        QString kind,path;
        quint64 seen=0,puts=0,takes=0,hits=0,misses=0,evictions=0,unused=0,refaults=0,rebuilds=0,replacements=0,quickReturns=0;
        qint64 admitted=-1,removed=-1,taken=-1,lastMiss=-1000,lastReject=-1000,bytes=0;
        QString removal,removalReason;
        bool resident=false,used=false,refaultPending=false;
    };
    QMap<QString,Item> identities,items;
    QMap<QString,quint64> counters;
    std::deque<QByteArray> ring;
    QByteArray pending;
    QString directory,path;
    QElapsedTimer clock;
    QTimer flushTimer;
    quint64 sequence=0,dropped=0,historyDropped=0,logDropped=0,logErrors=0,logWrites=0;
    int ringBytes=0;
    Impl() {
        clock.start(); flushTimer.setSingleShot(true); flushTimer.setInterval(250);
        QObject::connect(&flushTimer,&QTimer::timeout,this,[this] { flush(); });
    }
    void count(const QString& name,quint64 n=1) {
        if (counters.size()<128 || counters.contains(name)) counters[name]+=n;
    }
    Item& item(const QString& id) {
        if (!items.contains(id) && items.size()>=IdentityLimit) {
            auto oldest=items.begin();
            for (auto i=items.begin();i!=items.end();++i) if (i->seen<oldest->seen) oldest=i;
            items.erase(oldest); ++historyDropped;
        }
        auto& value=items[id]; value.seen=sequence+1; return value;
    }
    void flush() {
        if (pending.isEmpty() || path.isEmpty()) return;
        auto data=std::move(pending); pending.clear();
        struct stat dir{};
        if (::lstat(QFile::encodeName(directory).constData(),&dir) || !S_ISDIR(dir.st_mode) || dir.st_uid!=::geteuid() || (dir.st_mode&0077)) { ++logErrors; return; }
        const auto name=QFile::encodeName(path);
        int fd=::open(name.constData(),O_RDWR|O_APPEND|O_CLOEXEC|O_NOFOLLOW);
        if (fd>=0 && !ownedLog(fd)) { ::close(fd); ++logErrors; return; }
        struct stat s{};
        if (fd>=0 && (::fstat(fd,&s) || s.st_size+data.size()>LogLimit)) {
            ::close(fd); fd=-1;
            const auto backup=QFile::encodeName(path+".1");
            int previous=::open(backup.constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
            if (previous>=0) {
                const bool safe=ownedLog(previous); ::close(previous);
                if (!safe) { ++logErrors; return; }
            } else if (errno!=ENOENT) { ++logErrors; return; }
            if (::rename(name.constData(),backup.constData())) { ++logErrors; return; }
        }
        if (fd<0) {
            fd=::open(name.constData(),O_CREAT|O_EXCL|O_WRONLY|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
            if (fd<0) { ++logErrors; return; }
            data.prepend(Header);
        }
        const auto written=::write(fd,data.constData(),data.size());
        if (written!=data.size()) ++logErrors;
        else ++logWrites;
        ::close(fd);
    }
};
CacheDiagnostics::CacheDiagnostics():d(std::make_unique<Impl>()) {}
CacheDiagnostics::~CacheDiagnostics() { d->flush(); }
void CacheDiagnostics::setDirectory(const QString& directory) {
    d->directory=directory; d->path=QDir(directory).filePath("cache-churn-v1.jsonl");
}
void CacheDiagnostics::identify(const QString& key,const QString& kind,const QString& path) {
    if (key.isEmpty() || key.size()>80) return;
    if (!d->identities.contains(key) && d->identities.size()>=IdentityLimit) d->identities.erase(d->identities.begin());
    auto& identity=d->identities[key]; identity.kind=kind.left(24); identity.path=path.right(256);
}
void CacheDiagnostics::record(const QString& tier,const QString& action,const QString& key,qint64 bytes,
                              const QString& reason,qint64 elapsedUs,const QJsonObject& details) {
    if (tier.size()>24 || action.size()>32 || key.size()>80) return;
    d->count(tier+"."+action); if (elapsedUs>0) d->count(tier+"."+action+"_us",quint64(elapsedUs));
    const auto now=d->clock.elapsed(); QJsonObject extra=details;
    bool recordEvent=true;
    if (!key.isEmpty()) {
        auto& item=d->item(tier+":"+key);
        const auto identity=d->identities.constFind(key);
        if (identity!=d->identities.cend()) { item.kind=identity->kind; item.path=identity->path; }
        if (action=="admit" || action=="write") {
            ++item.puts;
            if (item.removal=="evict" || item.removal=="prune") {
                if (now-item.removed<=30000) { ++item.rebuilds; d->count(tier+".recent_readmissions"); extra["recent_readmission"]=true; }
            }
            if (item.taken>=0 && now-item.taken<=2000) { ++item.quickReturns; d->count(tier+".quick_returns"); extra["quick_return"]=true; }
            item.admitted=now; item.resident=true; item.used=false; item.bytes=bytes; item.removal.clear(); item.refaultPending=false; item.taken=-1;
        } else if (action=="take" || action=="hit") {
            if (action=="take") { ++item.takes; item.resident=false; item.taken=now; }
            else { ++item.hits; if (item.used) return; }
            if (item.admitted>=0) extra["residency_ms"]=double(now-item.admitted);
            item.used=true;
        } else if (action=="miss") {
            ++item.misses; recordEvent=now-item.lastMiss>=1000;
            if (item.refaultPending && item.removed>=0 && now-item.removed<=30000) {
                ++item.refaults; d->count(tier+".recent_refaults");
                d->count(tier+".refault_after_"+item.removal);
                extra["refault_after"]=item.removal; extra["since_removal_ms"]=double(now-item.removed); recordEvent=true;
                extra["removal_reason"]=item.removalReason;
                item.refaultPending=false;
            }
            if (recordEvent) item.lastMiss=now;
            else return;
        } else if (action=="evict" || action=="prune" || action=="discard" || action=="replace") {
            if (item.resident && !item.used) { ++item.unused; d->count(tier+".unused_removals"); d->count(tier+".unused_"+action); extra["unused"]=true; }
            if (item.admitted>=0) extra["residency_ms"]=double(now-item.admitted);
            if (action=="replace") ++item.replacements;
            else { ++item.evictions; item.removal=action; item.removalReason=reason.left(96); item.removed=now; item.refaultPending=true; }
            item.resident=false;
        } else if (action=="reject" || action=="bypass") {
            if (now-item.lastReject<1000) return;
            item.lastReject=now;
        }
        if (!item.path.isEmpty()) extra["source"]=item.path;
        if (!item.kind.isEmpty()) extra["kind"]=item.kind;
    }
    if (action=="prune_scan" && details["removed"].toInt()==0) d->count(tier+".empty_prune_scans");
    if (!recordEvent) return;
    QJsonObject event{{"seq",double(++d->sequence)},{"time",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"elapsed_ms",double(now)},{"pid",qint64(QCoreApplication::applicationPid())},{"tier",tier},{"action",action},
        {"bytes",double(bytes)},{"reason",reason.left(96)},{"work_us",double(elapsedUs)}};
    if (!key.isEmpty()) event["key"]=key;
    if (QJsonDocument(extra).toJson(QJsonDocument::Compact).size()<=1536) event["detail"]=extra;
    auto line=QJsonDocument(event).toJson(QJsonDocument::Compact)+'\n';
    while (!d->ring.empty() && (d->ring.size()>=EventLimit || d->ringBytes+line.size()>EventBytesLimit)) {
        d->ringBytes-=d->ring.front().size(); d->ring.pop_front(); ++d->dropped;
    }
    d->ringBytes+=line.size(); d->ring.push_back(line);
    if (!d->path.isEmpty()) {
        if (d->pending.size()+line.size()<=PendingLimit) d->pending+=line;
        else ++d->logDropped;
        if (!d->flushTimer.isActive()) d->flushTimer.start();
    }
}
QJsonObject CacheDiagnostics::summary() const {
    QJsonObject counters; for (auto i=d->counters.begin();i!=d->counters.end();++i) counters[i.key()]=double(i.value());
    QList<std::pair<QString,Impl::Item>> items;
    for (auto i=d->items.begin();i!=d->items.end();++i) if (i->refaults || i->unused || i->replacements || i->rebuilds) items.append({i.key(),i.value()});
    auto score=[](const Impl::Item& i) { return i.refaults*8+i.rebuilds*8+i.unused*4+i.replacements; };
    std::sort(items.begin(),items.end(),[&](const auto& a,const auto& b) { return score(a.second)>score(b.second); });
    QJsonArray suspects;
    for (int i=0;i<std::min(8,int(items.size()));++i) {
        const auto& [id,item]=items[i];
        suspects.append(QJsonObject{{"identity",id},{"source",item.path},{"kind",item.kind},{"admissions",double(item.puts)},
            {"takes",double(item.takes)},{"hits",double(item.hits)},{"misses",double(item.misses)},{"unused_removals",double(item.unused)},
            {"recent_refaults",double(item.refaults)},{"recent_readmissions",double(item.rebuilds)},{"replacements",double(item.replacements)},
            {"quick_returns",double(item.quickReturns)}});
    }
    return {{"counters",counters},{"suspects",suspects},{"history_entries",d->items.size()},{"history_limit",IdentityLimit},
        {"history_dropped",double(d->historyDropped)},{"event_count",int(d->ring.size())},{"event_bytes",d->ringBytes},
        {"event_limit",EventLimit},{"event_byte_limit",EventBytesLimit},{"events_dropped",double(d->dropped)},
        {"last_seq",double(d->sequence)},{"log_path",d->path},{"log_limit",LogLimit},{"log_files",2},
        {"log_pending_bytes",d->pending.size()},{"log_pending_limit",PendingLimit},{"log_events_dropped",double(d->logDropped)},
        {"log_errors",double(d->logErrors)},{"log_writes",double(d->logWrites)}};
}
QJsonObject CacheDiagnostics::events(int limit,quint64 after) const {
    QJsonArray events; limit=std::clamp(limit,1,EventLimit);
    for (const auto& line:d->ring) {
        const auto event=QJsonDocument::fromJson(line).object();
        if (event["seq"].toDouble()<=double(after)) continue;
        events.append(event);
        if (events.size()>limit) events.removeFirst();
    }
    return {{"events",events},{"last_seq",double(d->sequence)},{"events_dropped",double(d->dropped)}};
}
}
