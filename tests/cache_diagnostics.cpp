#include "../src/cache_diagnostics.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using namespace preview;
void expect(bool value,const char* why) { if (!value) throw std::runtime_error(why); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv); QTemporaryDir temp;
    CacheDiagnostics d; d.identify("same","image","/assets/small.png");
    d.record("ram","admit","same",4096,"view_unload");
    d.record("ram","take","same",4096,"visible_view");
    d.record("ram","admit","same",4096,"view_unload");
    auto counters=d.summary()["counters"].toObject();
    expect(counters["ram.quick_returns"].toInt()==1 && counters["ram.unused_removals"].toInt()==0,"normal transfers are not unused eviction churn");
    d.record("ram","evict","same",4096,"ram_bytes");
    d.record("ram","miss","same",0,"visible_view"); d.record("ram","miss","same",0,"visible_view");
    d.record("ram","admit","same",4096,"view_unload");
    counters=d.summary()["counters"].toObject();
    expect(counters["ram.recent_refaults"].toInt()==1 && counters["ram.recent_readmissions"].toInt()==1,"one eviction yields one refault and one readmission");
    expect(counters["ram.unused_evict"].toInt()==1,"unused eviction distinguished");
    const auto events=d.events()["events"].toArray(); bool reason=false;
    for (const auto& event:events) if (event.toObject()["detail"].toObject()["removal_reason"]=="ram_bytes") reason=true;
    expect(reason,"refault includes original limiting constraint");
    d.record("ram","hit","same",4096,"thumbnail"); const auto before=d.summary()["last_seq"];
    for (int i=0;i<1000;++i) d.record("ram","hit","same",4096,"thumbnail");
    expect(d.summary()["last_seq"]==before,"repeated painting hits are counters only");
    for (int i=0;i<400;++i) {
        const auto key=QString::number(i); d.identify(key,"thumbnail",QString(4096,'x'));
        d.record("ram","admit",key,4096,"thumbnail"); d.record("ram","evict",key,4096,"ram_entries");
    }
    const auto summary=d.summary();
    expect(summary["history_entries"].toInt()<=CacheDiagnostics::IdentityLimit && summary["history_dropped"].toInt()>0,"per-key history bounded");
    expect(summary["event_count"].toInt()<=CacheDiagnostics::EventLimit && summary["event_bytes"].toInt()<=CacheDiagnostics::EventBytesLimit,"event count and bytes bounded");
    expect(d.events(3)["events"].toArray().size()==3,"event tail limit");
    const auto last=summary["last_seq"].toDouble(); expect(d.events(128,quint64(last))["events"].toArray().isEmpty(),"sequence filtering");
    for (int batch=0;batch<20;++batch) {
        CacheDiagnostics file; file.setDirectory(temp.path());
        for (int i=0;i<240;++i) file.record("ram","evict",QString::number(i),4096,QString(90,'q'));
        expect(file.summary()["log_pending_bytes"].toInt()<=CacheDiagnostics::PendingLimit,"file write batch bounded");
        expect(file.summary()["log_events_dropped"].toInt()>0,"overflow is reported");
    }
    const auto path=temp.path()+"/cache-churn-v1.jsonl";
    expect(QFile::exists(path+".1"),"log rotation occurred");
    qint64 total=0;
    for (const auto& suffix:{QString{},QString(".1")}) {
        QFile file(path+suffix); expect(file.open(QIODevice::ReadOnly),"owned log readable"); total+=file.size();
        expect(file.size()<=CacheDiagnostics::LogLimit,"log file bound");
        while (!file.atEnd()) { QJsonParseError parse; QJsonDocument::fromJson(file.readLine(),&parse); expect(parse.error==QJsonParseError::NoError,"complete JSON lines"); }
    }
    expect(total<=2*CacheDiagnostics::LogLimit,"combined persistent log bound");
    const auto other=temp.path()+"/other"; expect(QDir().mkdir(other),"unowned log fixture directory");
    expect(QFile::setPermissions(other,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner),"private unowned log fixture");
    { QFile foreign(other+"/cache-churn-v1.jsonl"); expect(foreign.open(QIODevice::WriteOnly),"foreign log fixture"); foreign.write("preserve"); }
    expect(QFile::setPermissions(other+"/cache-churn-v1.jsonl",QFile::ReadOwner|QFile::WriteOwner),"private foreign file");
    { CacheDiagnostics safe; safe.setDirectory(other); safe.record("heap","trim",{},0,"test"); }
    { QFile foreign(other+"/cache-churn-v1.jsonl"); expect(foreign.open(QIODevice::ReadOnly) && foreign.readAll()=="preserve","foreign logs never overwritten"); }
    std::cout<<"Cache churn classification, bounded history, hit suppression, log batching, rotation and ownership passed\n";
}
