#include "thumbnail_queue.h"
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <iostream>
#include <stdexcept>

static void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
static void settle(int ms) { QEventLoop loop; QTimer::singleShot(ms,&loop,&QEventLoop::quit); loop.exec(); }
template<class F> static void until(F condition,int limit=4000) {
    QElapsedTimer clock; clock.start();
    while(!condition() && clock.elapsed()<limit)settle(2);
    require(condition(),"Queue condition timed out");
}
int main(int argc,char** argv) {
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--thumbnail-job") {
        QCoreApplication app(argc,argv); QThread::msleep(500);
        QImage image(160,100,QImage::Format_RGBA8888); image.fill(Qt::red);
        return image.save(QString::fromLocal8Bit(argv[2])+"/thumbnail.png")?0:1;
    }
    QApplication app(argc,argv); app.setQuitOnLastWindowClosed(false);
    try {
        QTemporaryDir root; require(root.isValid(),"Missing scratch");
        qputenv("ASSET_PREVIEW_SMALL_CACHE",root.filePath("cache").toUtf8());
        qputenv("ASSET_PREVIEW_TEXTURE_CACHE",root.filePath("textures").toUtf8());
        preview::PreviewCache cache; ThumbnailQueue queue(&cache,root.path());
        QImage pixels(2048,2048,QImage::Format_RGBA8888); pixels.fill(QColor(30,140,220));
        const auto path=root.filePath("image.png"); require(pixels.save(path),"Cannot save pixels");
        QList<ThumbnailQueue::Item> items{{"model",path,"model"},{"first",path,"image"},{"second",path,"image"},
            {"background",path,"model"},{"urgent",path,"model"},{"alias",path,"image"},{"spare",path,"image"}};
        QMap<QString,QString> identities; for(const auto& item:items)identities[item.id]=item.id;
        QStringList visible{"model","first","second","alias"};
        for(int i=0;i<6;++i) {
            const auto id=QString("extra-%1").arg(i);
            items.append({id,path,"image"}); identities[id]=id; visible.append(id);
        }
        identities["alias"]="first";
        QStringList lookups;
        queue.keyFor=[&](const QString& id) { lookups.append(id); return identities.value(id); };
        queue.configFor=[](const QString&) { return QJsonObject{{"kind","model"}}; };
        bool refreshed=false; QElapsedTimer invalidated; QSet<QString> previous; QString failure;
        queue.activityChanged=[&] {
            if(!refreshed && queue.metrics()["decoded"].toInt()==1) {
                refreshed=true; previous=queue.loadingIds(); invalidated.start(); queue.refresh();
            } else if(refreshed && invalidated.elapsed()<20 && !(queue.loadingIds()-previous).isEmpty())
                failure="Completion bypassed a genuine source/viewport debounce";
        };
        queue.setItems(items); queue.demand(visible,{},{},true,true);
        settle(80); require(cache.peekThumbnail("spare").isNull(),"Background work bypassed the quiet period");
        until([&] { return queue.metrics()["decoded"].toInt()==8; });
        require(refreshed && failure.isEmpty(),"Completion bypassed the viewport debounce");
        require(queue.metrics()["worker_id"].toString()=="model","Images waited behind the model job");
        require(lookups.indexOf("first")<lookups.indexOf("model") && lookups.indexOf("second")<lookups.indexOf("model"),"Visible image preparation did not lead");
        require(queue.metrics()["image_peak"].toInt()==2,"Independent images did not overlap");
        require(!cache.peekThumbnail("first").isNull() && !cache.peekThumbnail("second").isNull(),"Images failed to publish");
        require(queue.metrics()["decoded"].toInt()==8,"An alias was decoded twice");
        until([&] { return queue.metrics()["worker_id"].toString()=="background"; });
        const auto backgroundPid=queue.metrics()["worker_pid"].toDouble(); require(backgroundPid>0,"No background child");
        queue.demand({"urgent"},{},{},true,true);
        until([&] { return queue.metrics()["worker_id"].toString()=="urgent"; });
        require(!QFileInfo::exists(QString("/proc/%1").arg(qint64(backgroundPid))),"Visible work did not preempt the exact background child");
        queue.demand({}, {}, {}, false, false); cache.clear();
        until([&] { return !queue.metrics()["worker_active"].toBool(); }); settle(550);
        require(cache.metrics()["entries"].toInt()==0,"Hidden completion repopulated the RAM cache");
        require(QDir(root.path()).entryList({"job-*"},QDir::Dirs|QDir::NoDotAndDotDot).isEmpty(),"Cancelled child scratch survived");
        // Only raster work in this phase. Warming is serial and uses spare
        // capacity, but its complete finite queue eventually fills the cache.
        items={{"first",path,"image"},{"second",path,"image"},{"alias",path,"image"},{"spare",path,"image"}};
        queue.setItems(items); queue.demand({}, {"spare"}, {}, true,true);
        until([&] { return !queue.metrics()["worker_active"].toBool(); });
        require(cache.metrics()["entries"].toInt()==3,"All-open warming did not finish or alias was duplicated");
        const auto before=queue.metrics()["decoded"].toInt();
        identities["first"]="replacement"; identities["alias"]="replacement"; queue.refresh();
        until([&] { return !queue.metrics()["worker_active"].toBool(); });
        require(!cache.peekThumbnail("replacement").isNull(),"Background source invalidation did not publish");
        require(queue.metrics()["decoded"].toInt()==before+1,"Source replacement decoded aliases more than once");
        queue.demand({}, {"spare"}, {"first","alias"}, true,true);
        identities["first"]="after-active-view"; identities["alias"]="after-active-view";
        queue.demand({}, {"spare"}, {}, true,true);
        until([&] { return !queue.metrics()["worker_active"].toBool(); });
        require(!cache.peekThumbnail("after-active-view").isNull(),"A departing active view retained its stale visual identity");
        cache.clear(true); queue.refresh(true);
        queue.demand({}, {"spare"}, {}, true,false);
        queue.demand({}, {"spare"}, {}, true,true); settle(350);
        require(cache.metrics()["entries"].toInt()==0,"Explicit clearing immediately refilled background cache");
        queue.demand({"first"},{},{},true,true);
        until([&] { return !queue.metrics()["worker_active"].toBool(); });
        require(!cache.peekThumbnail("after-active-view").isNull(),"Navigation did not resume after explicit clearing");
        require(queue.metrics()["image_worker_limit"].toInt()==2 && queue.metrics()["render_worker_limit"].toInt()==1,"Worker bounds changed");
        std::cout<<"Parallel image/model lanes, images first, deduplication, quiet warming, foreground preemption, hidden cancellation, freshness and clear/resume passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
