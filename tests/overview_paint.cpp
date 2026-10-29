#include "overview_grid.h"
#include "native_overlay.h"
#include "theme.h"
#include <QApplication>
#include <QBackingStore>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOffscreenSurface>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScrollBar>
#include <QStyle>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <stdexcept>

static void require(bool ok,const QString& error) { if(!ok)throw std::runtime_error(error.toStdString()); }
static void settle(int ms=20) { QEventLoop loop; QTimer::singleShot(ms,&loop,&QEventLoop::quit); loop.exec(); }
static QImage retained(QWidget& window,QWidget& viewport) {
    auto* image=dynamic_cast<QImage*>(window.backingStore()->paintDevice());
    require(image,"The raster backing store is unavailable");
    const auto ratio=image->devicePixelRatio();
    const auto origin=viewport.mapTo(&window,QPoint())*ratio;
    return image->copy(QRect(origin,viewport.size()*ratio));
}
int main(int argc,char** argv) {
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc,argv);
    const auto requestedStyle=qEnvironmentVariable("ASSET_PREVIEW_TEST_STYLE","Fusion");
    if(!app.setStyle(requestedStyle)) { std::cerr<<"Missing installed style\n"; return 1; }
    Theme::apply(qEnvironmentVariable("ASSET_PREVIEW_TEST_THEME","auto"));
    app.setQuitOnLastWindowClosed(false);
    QTemporaryDir root; qputenv("ASSET_PREVIEW_SMALL_CACHE",root.filePath("cache").toUtf8());
    try {
        QOpenGLContext context; require(context.create(),"Cannot verify hardware renderer");
        QOffscreenSurface surface; surface.setFormat(context.format()); surface.create();
        require(context.makeCurrent(&surface),"Cannot query renderer");
        const auto renderer=QString::fromUtf8(reinterpret_cast<const char*>(context.functions()->glGetString(GL_RENDERER)));
        require(!renderer.isEmpty() && !renderer.contains("llvmpipe",Qt::CaseInsensitive),"Software renderer"); context.doneCurrent();
        preview::PreviewCache cache; QWidget window; window.resize(815,530);
        OverviewGrid grid(&cache,root.path(),&window); grid.setGeometry(window.rect());
        QWidget sibling(&window),overlay(&sibling);
        setNativeOverlay(&overlay); overlay.hide();
        QList<OverviewGrid::Item> items;
        for(int i=0;i<80;++i) {
            const auto id=QString("tile-%1").arg(i); QImage thumb(160,100,QImage::Format_RGB32);
            for(int y=0;y<thumb.height();++y)for(int x=0;x<thumb.width();++x)
                thumb.setPixelColor(x,y,((x/9+y/7)%2)?QColor(30+i*2,110,200):QColor(180,60+i,40));
            cache.putThumbnail(id,thumb);
            items.append({id,"A long reference label "+id,{},"graph",QString::number(i/16),"Project "+QString::number(i/16)});
        }
        grid.keyFor=[](const QString& id){return id;}; grid.setItems(items,"tile-0",true);
        window.show(); overlay.show(); settle(40);
        require(overlay.internalWinId() && !grid.internalWinId() && !sibling.internalWinId(),"Native overlays promoted the raster interface");
        sibling.hide(); settle(180);
        const bool wayland=QGuiApplication::platformName()=="wayland";
        require(!wayland || app.arguments().contains("--compositor"),"Wayland verification requires compositor captures");
        int states=0,retainedFrames=0,compositorFrames=0;
        const auto verify=[&](const QString& label) {
            ++states;
            if(wayland) { settle(80); return; }
            settle(80); const auto before=retained(window,*grid.viewport());
            grid.viewport()->repaint(); const auto after=retained(window,*grid.viewport());
            bool differs=before.size()!=after.size();
            if(before!=after && !differs) {
                const auto a=before.convertToFormat(QImage::Format_RGB32),b=after.convertToFormat(QImage::Format_RGB32);
                // The outer physical pixel can straddle a fractional viewport
                // edge; compare the fully covered interior, including card gaps.
                for(int y=1;y<a.height()-1 && !differs;++y) {
                    const auto* left=reinterpret_cast<const QRgb*>(a.constScanLine(y));
                    const auto* right=reinterpret_cast<const QRgb*>(b.constScanLine(y));
                    for(int x=1;x<a.width()-1;++x)if(std::max({std::abs(qRed(left[x])-qRed(right[x])),std::abs(qGreen(left[x])-qGreen(right[x])),std::abs(qBlue(left[x])-qBlue(right[x]))})>2) { differs=true; break; }
                }
            }
            if(differs) {
                const auto out=qEnvironmentVariable("ASSET_PREVIEW_EVIDENCE_DIR",root.path());
                QDir().mkpath(out); before.save(out+"/"+label+"-incremental.png"); after.save(out+"/"+label+"-clean.png");
                throw std::runtime_error(("Incremental paint differs from a clean frame: "+label).toStdString());
            }
            ++retainedFrames;
        };
        const auto compositor=[&](const QString& label) {
            if(!app.arguments().contains("--compositor"))return;
            settle(80);
            const auto out=qEnvironmentVariable("ASSET_PREVIEW_EVIDENCE_DIR",root.path()); QDir().mkpath(out);
            const auto path=out+"/"+label+"-compositor.png";
            QProcess screenshot; auto environment=QProcessEnvironment::systemEnvironment(); environment.remove("QT_SCALE_FACTOR");
            screenshot.setProcessEnvironment(environment); screenshot.start("spectacle",{"-b","-n","-a","-e","-S","-o",path});
            require(screenshot.waitForFinished(15000) && screenshot.exitCode()==0,"Compositor capture failed: "+QString::fromUtf8(screenshot.readAllStandardError()));
            // Wayland rotates native backing buffers after flush. Capture the
            // compositor first, then render an independent reference image.
            const auto expected=grid.viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
            expected.save(out+"/"+label+"-reference.png");
            const QImage capture(path); const auto ratio=window.devicePixelRatioF();
            const auto actual=capture.copy(QRect(grid.viewport()->mapTo(&window,QPoint())*ratio,grid.viewport()->size()*ratio)).convertToFormat(QImage::Format_RGB32);
            require(actual.size()==expected.size(),"Compositor capture size differs from the painted viewport");
            qint64 different=0;
            for(int y=1;y<actual.height()-1;++y)for(int x=1;x<actual.width()-1;++x) {
                const auto a=actual.pixel(x,y),b=expected.pixel(x,y);
                if(std::max({std::abs(qRed(a)-qRed(b)),std::abs(qGreen(a)-qGreen(b)),std::abs(qBlue(a)-qBlue(b))})>3)++different;
            }
            require(different<double(actual.width()*actual.height())*.001,"Compositor pixels differ from the painted viewport");
            ++compositorFrames;
        };
        verify("initial");
        compositor("initial");
        grid.setMarked({"tile-0","tile-2"}); verify("checked");
        grid.setMarked({"tile-1"}); verify("changed-checks");
        for(int offset:{1,2,3,7,19,33,74,157,158,159,173,316,317,321,109,7,0}) {
            grid.verticalScrollBar()->setValue(offset); verify(QString("scroll-%1").arg(offset));
            const auto visible=grid.describe(grid.viewport())["tiles"].toArray();
            if(!visible.isEmpty()) {
                const auto tile=visible.last().toObject();
                grid.select(tile["id"].toString()); verify(QString("selection-%1").arg(offset));
                const auto middle=visible[visible.size()/2].toObject();
                const QPointF point(middle["x"].toInt()+20,middle["y"].toInt()+40);
                QMouseEvent move(QEvent::MouseMove,point,point,Qt::NoButton,Qt::NoButton,Qt::NoModifier);
                QApplication::sendEvent(grid.viewport(),&move); verify(QString("hover-%1").arg(offset));
                const auto box=middle["checkbox"].toObject();
                const QPointF checkPoint(box["x"].toInt()+14,box["y"].toInt()+14);
                QMouseEvent checkMove(QEvent::MouseMove,checkPoint,checkPoint,Qt::NoButton,Qt::NoButton,Qt::NoModifier);
                QApplication::sendEvent(grid.viewport(),&checkMove); verify(QString("checkbox-hover-%1").arg(offset));
                QApplication::sendEvent(grid.viewport(),&move); verify(QString("checkbox-leave-%1").arg(offset));
                QEvent leave(QEvent::Leave); QApplication::sendEvent(grid.viewport(),&leave); verify(QString("leave-%1").arg(offset));
            }
        }
        for(int width:{444,1024,815}) { window.resize(width,530); grid.setGeometry(window.rect()); verify(QString("resize-%1").arg(width)); }
        compositor("scrolled");
        // Updating an earlier group must keep the same visible content anchored.
        grid.verticalScrollBar()->setValue(700); settle();
        const auto anchor=grid.describe(grid.viewport())["tiles"].toArray().first().toObject();
        for(int i=0;i<5;++i)items.insert(0,{QString("insert-%1").arg(i),"New output",{},"graph","0","Project 0"});
        grid.setItems(items,"tile-0",true); settle();
        const auto visible=grid.describe(grid.viewport())["tiles"].toArray();
        bool anchored=false;
        for(const auto& value:visible) { const auto tile=value.toObject(); if(tile["id"]==anchor["id"])anchored=tile["y"]==anchor["y"]; }
        require(anchored,"Adding assets above the viewport shifted the visible content");
        verify("insert");
        compositor("insert");
        std::cout<<QJsonDocument(QJsonObject{{"renderer",renderer},{"scale",window.devicePixelRatioF()},{"style",app.style()->objectName()},
            {"theme",Theme::effective()},{"interaction_states",states},{"retained_frames",retainedFrames},{"compositor_frames",compositorFrames}}).toJson(QJsonDocument::Compact).constData()<<'\n';
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
