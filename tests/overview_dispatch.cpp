#include "overview_grid.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonArray>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <iostream>
#include <stdexcept>

static void require(bool ok,const QString& error) { if(!ok)throw std::runtime_error(error.toStdString()); }
static void settle(int ms) {
    QEventLoop loop; QTimer::singleShot(ms,&loop,&QEventLoop::quit); loop.exec();
}
static void suspensionAndDebounce() {
    QTemporaryDir root; require(root.isValid(),"Cannot create dispatch fixture");
    qputenv("ASSET_PREVIEW_SMALL_CACHE",root.filePath("cache").toUtf8());
    qputenv("ASSET_PREVIEW_TEXTURE_CACHE",root.filePath("textures").toUtf8());
    QImage image(64,64,QImage::Format_RGBA8888); image.fill(Qt::cyan);
    const auto path=root.filePath("image.png"); require(image.save(path),"Cannot write fixture");
    preview::PreviewCache cache; QWidget window; window.resize(500,260);
    OverviewGrid grid(&cache,{},&window); grid.setGeometry(window.rect());
    grid.keyFor=[&](const QString&) { return preview::sourceCacheKey("fixture",path); };
    grid.setItems({{"image","Image",path,"image",{},{}}},"image");
    window.show(); settle(5); grid.hide(); settle(40);
    require(cache.metrics()["entries"].toInt()==0,"Hidden grid dispatched a pending thumbnail");
    require(!grid.describe(&grid)["worker_active"].toBool(),"Hidden grid retained work");
    grid.show(); settle(5); grid.refresh();
    settle(10); require(cache.metrics()["entries"].toInt()==0,"Refresh bypassed viewport debounce");
    QElapsedTimer clock; clock.start();
    while(grid.describe(&grid)["worker_active"].toBool() && clock.elapsed()<2000)settle(2);
    require(cache.metrics()["entries"].toInt()==1,"Reentry did not complete");
    window.showMinimized(); grid.invalidate();
    require(!grid.describe(&grid)["worker_active"].toBool(),"Minimized grid retained dispatch");
    window.showNormal(); grid.invalidate(); settle(60);
    require(!grid.describe(&grid)["worker_active"].toBool(),"Restore did not settle");
    std::cout<<"Debounce, hide/reentry and minimize/restore passed\n";
}
static void stableThumbnails() {
    QTemporaryDir root; require(root.isValid(),"Cannot create paint fixture");
    qputenv("ASSET_PREVIEW_SMALL_CACHE",root.filePath("cache").toUtf8());
    qputenv("ASSET_PREVIEW_TEXTURE_CACHE",root.filePath("textures").toUtf8());
    QImage image(160,100,QImage::Format_RGBA8888); image.fill(QColor(40,130,210));
    const auto path=root.filePath("source.png"); require(image.save(path),"Cannot write paint fixture");
    preview::PreviewCache cache;
    QWidget window; window.resize(580,430);
    OverviewGrid grid(&cache,root.path(),&window); grid.setGeometry(window.rect());
    QMap<QString,QString> identities;
    QList<OverviewGrid::Item> items{{"slow","Slow import",path,"model","a","First project"}};
    identities["slow"]="slow";
    for(int i=0;i<30;++i) {
        const auto id=QString("cached-%1").arg(i);
        identities[id]=id; cache.putThumbnail(id,image);
        items.append({id,"Cached reference "+id,path,"image",i<12?"a":"b",i<12?"First project":"Second project"});
    }
    grid.keyFor=[&](const QString& id) { return identities.value(id); };
    grid.configFor=[](const QString&) { return QJsonObject{{"kind","model"}}; };
    grid.setItems(items,"cached-0",true); window.show();
    QElapsedTimer timer; timer.start();
    while(!grid.describe(&grid)["worker_pid"].toDouble() && timer.elapsed()<2000)settle(1);
    require(grid.describe(&grid)["worker_pid"].toDouble()>0,"Slow import fixture did not start");
    const auto sample=[&](const QString& id) {
        for(const auto& value:grid.describe(grid.viewport())["tiles"].toArray()) {
            const auto tile=value.toObject(); if(tile["id"].toString()!=id)continue;
            return grid.viewport()->grab().toImage().pixelColor(tile["x"].toInt()+tile["width"].toInt()/2,tile["y"].toInt()+40);
        }
        throw std::runtime_error("Expected cached tile is offscreen");
    };
    const auto color=QColor(40,130,210);
    require(sample("cached-0")==color,"Cached tiles waited behind a slow thumbnail job");
    grid.refresh();
    require(sample("cached-0")==color,"Refresh flashed a cached tile to a placeholder");
    grid.verticalScrollBar()->setValue(20);
    require(sample("cached-0")==color,"Scrolling flashed an overlapping cached tile");
    window.resize(600,450); grid.setGeometry(window.rect());
    require(sample("cached-0")==color,"Resizing flashed a cached tile");
    items[1].label="Renamed reference"; grid.setItems(items,"cached-0",true);
    require(sample("cached-0")==color,"Metadata update flashed a cached tile");
    image.fill(QColor(180,80,60)); cache.putThumbnail("replacement",image); identities["cached-0"]="replacement";
    grid.refresh();
    require(sample("cached-0")==QColor(180,80,60),"Refresh did not validate a changed visible source");
    grid.verticalScrollBar()->setValue(grid.verticalScrollBar()->maximum()); settle(40);
    image.fill(Qt::green); cache.putThumbnail("offscreen-replacement",image); identities["cached-0"]="offscreen-replacement";
    grid.verticalScrollBar()->setValue(0); settle(40);
    require(sample("cached-0")==QColor(Qt::green),"Returning to an unwatched source reused its stale key");
    grid.hide();
    std::cout<<"Cached thumbnails survive slow jobs, refresh, scroll, resize and metadata changes; sources revalidate\n";
}
int main(int argc,char** argv) {
    // Finite child fixture exercises a busy import without an engine or GPU.
    if(argc>1 && QString::fromLocal8Bit(argv[1])=="--thumbnail-job") { QThread::msleep(500); return 1; }
    QApplication app(argc,argv); app.setQuitOnLastWindowClosed(false);
    try { suspensionAndDebounce(); stableThumbnails(); }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
