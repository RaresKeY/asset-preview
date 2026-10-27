#include "overview_grid.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <stdexcept>

static void require(bool ok,const QString& error) { if(!ok)throw std::runtime_error(error.toStdString()); }
static void exercise(int mode) {
    const bool reenter=mode==1;
    const QString action=mode==3?"Minimize/restore":mode==2?"Refresh during decode":reenter?"Reentry":"Refresh";
    QTemporaryDir root; require(root.isValid(),"Cannot create dispatch fixture");
    qputenv("ASSET_PREVIEW_SMALL_CACHE",root.filePath("cache").toUtf8());
    qputenv("ASSET_PREVIEW_TEXTURE_CACHE",root.filePath("textures").toUtf8());
    QImage image(64,64,QImage::Format_RGBA8888); image.fill(Qt::cyan);
    const auto first=root.filePath("first.png"),second=root.filePath("second.png");
    require(image.save(first) && image.save(second),"Cannot write fixture images");
    preview::PreviewCache cache;
    QWidget window; window.resize(500,260);
    OverviewGrid grid(&cache,{},&window); grid.setGeometry(window.rect());
    QEventLoop loop; QElapsedTimer invalidated;
    int firstLookups=0; bool changed=false,secondRequested=false;
    qint64 nextDelay=-1; QString failure;
    grid.keyFor=[&](const QString& id) {
        if(id=="first" && ++firstLookups==(mode==2?1:2)) {
            // Invalidate on the next GUI turn, either after the worker starts
            // or after its publication has queued the fast continuation.
            QTimer::singleShot(0,&grid,[&] {
                changed=true; invalidated.start();
                if(reenter || mode==3) {
                    if(mode==3) { window.showMinimized(); grid.invalidate(); }
                    else grid.hide();
                    if(grid.describe(&grid)["worker_active"].toBool())failure="Suspended grid retained a pending dispatch";
                    if(mode==3) {
                        // Allow Qt's native minimize events to settle before
                        // restoring, as a real window-manager action would.
                        QTimer::singleShot(40,&grid,[&] {
                            if(secondRequested || grid.describe(&grid)["worker_active"].toBool())
                                failure="Minimized grid dispatched another tile";
                            invalidated.restart(); window.showNormal();
                            // PreviewWindow invalidates on both state changes.
                            grid.invalidate();
                        });
                    } else grid.show();
                } else grid.refresh();
            });
        }
        if(id=="second" && changed && !secondRequested) {
            secondRequested=true; nextDelay=invalidated.elapsed();
        }
        return preview::sourceCacheKey("dispatch-fixture",id=="first"?first:second);
    };
    grid.setItems({{"first","First",first,"image",{},{}},{"second","Second",second,"image",{},{}}},"first");
    window.show();
    QTimer poll; poll.setInterval(2);
    QObject::connect(&poll,&QTimer::timeout,&loop,[&] {
        if(secondRequested && !grid.describe(&grid)["worker_active"].toBool())loop.quit();
    });
    QTimer timeout; timeout.setSingleShot(true);
    QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
    poll.start(); timeout.start(3000); loop.exec(); poll.stop();
    require(failure.isEmpty(),failure);
    require(secondRequested,QString("%1 did not resume (visible=%2 minimized=%3 first-lookups=%4 state=%5)")
        .arg(action).arg(grid.isVisible()).arg(window.isMinimized()).arg(firstLookups)
        .arg(QString::fromUtf8(QJsonDocument(grid.describe(&grid)).toJson(QJsonDocument::Compact))));
    require(nextDelay>=20,QString("%1 bypassed debounce: next tile after %2 ms").arg(action).arg(nextDelay));
    require(!grid.describe(&grid)["worker_active"].toBool(),"Dispatch did not settle");
    require(cache.metrics()["entries"].toInt()==2,"Missing completed thumbnails");
    std::cout<<action.toStdString()<<" postponed queued continuation: "<<nextDelay<<" ms\n";
}
int main(int argc,char** argv) {
    QApplication app(argc,argv); app.setQuitOnLastWindowClosed(false);
    try { exercise(0); exercise(1); exercise(2); exercise(3); }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
