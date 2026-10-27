#include "overview_grid.h"
#include "project_identity.h"
#include <QImageReader>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRunnable>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionButton>
#include <QToolTip>
#include <QTimer>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>

namespace {
constexpr int Gap=12,TileWidth=172,TileHeight=146;
QImage smallImage(const QString& path) {
    // Refuse expensive inputs; already-opened large assets use their saved frame.
    if (QFileInfo(path).size()>16*1024*1024 || !QFileInfo(path).isFile()) return {};
    QImageReader reader(path); reader.setAutoTransform(true);
    static const QSet<QByteArray> formats{"png","jpeg","webp","bmp","tiff"};
    if (!formats.contains(reader.format().toLower())) return {};
    const auto size=reader.size();
    const auto format=reader.imageFormat();
    const int pixelBytes=format==QImage::Format_Invalid?16:std::max(4,int(QImage::toPixelFormat(format).bitsPerPixel()+7)/8);
    if (!size.isValid() || qint64(size.width())*size.height()*pixelBytes>32*1024*1024) return {};
    reader.setScaledSize(size.scaled(256,160,Qt::KeepAspectRatio));
    return reader.read().convertToFormat(QImage::Format_RGBA8888);
}
}
OverviewGrid::OverviewGrid(preview::PreviewCache* store,const QString& jobDirectory,QWidget* parent):QAbstractScrollArea(parent),cache(store),renderRoot(jobDirectory) {
    setObjectName("overviewGrid"); setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus); setAccessibleName("All open previews");
    viewport()->setMouseTracking(true); worker.setMaxThreadCount(1); worker.setExpiryTimeout(1000);
    worker.setThreadPriority(QThread::LowPriority);
    dispatch.setSingleShot(true);
    connect(&dispatch,&QTimer::timeout,this,[this] { work(); });
    spinner.setInterval(80);
    connect(&spinner,&QTimer::timeout,this,[this] {
        spinAngle=(spinAngle+30)%360;
        for(int i=0;i<items.size();++i)if(items[i].id==loadingId) { viewport()->update(tile(i)); break; }
    });
    renderDeadline=new QTimer(this); renderDeadline->setSingleShot(true);
    connect(renderDeadline,&QTimer::timeout,this,[this] { renderTimedOut=true; cancelRender(); });
    connect(verticalScrollBar(),&QScrollBar::valueChanged,this,[this] {
        keys.clear();
        if (!renderingId.isEmpty() && !visibleIds().contains(renderingId)) cancelRender();
        viewport()->update(); schedule(); if (viewportChanged) viewportChanged();
    });
}
OverviewGrid::~OverviewGrid() {
    ++epoch; dispatch.stop(); cancelRender();
    if (renderWorker) { renderWorker->disconnect(this); renderWorker->waitForFinished(1000); }
    worker.clear(); worker.waitForDone();
}
int OverviewGrid::columns() const { return std::max(1,(viewport()->width()-Gap)/(TileWidth+Gap)); }
QRect OverviewGrid::tile(int i) const {
    return tiles.value(i).translated(0,-verticalScrollBar()->value());
}
QRect OverviewGrid::checkbox(int i) const {
    const auto r=tile(i);
    return {r.right()-33,r.top()+6,28,28};
}
void OverviewGrid::setMarked(const QSet<QString>& ids) {
    if (checked==ids) return;
    checked=ids; viewport()->update();
}
void OverviewGrid::setItems(QList<Item> next,const QString& id,bool groups) {
    bool changed=items.size()!=next.size() || grouped!=groups;
    grouped=groups;
    if (!changed) for (int i=0;i<items.size();++i) if (items[i].id!=next[i].id || items[i].path!=next[i].path || items[i].label!=next[i].label || items[i].kind!=next[i].kind || items[i].project!=next[i].project || items[i].projectLabel!=next[i].projectLabel) { changed=true; break; }
    if (changed) { items=std::move(next); keys.clear(); attempted.clear(); ++epoch; cancelRender(); reflow(); viewport()->update(); }
    QString target=id;
    if (std::none_of(items.begin(),items.end(),[&](const auto& item) { return item.id==target; })) target=items.isEmpty()?QString():items.first().id;
    select(target); schedule();
}
void OverviewGrid::select(const QString& id,bool reveal) {
    const auto previous=current;
    if (id.isEmpty() || std::any_of(items.begin(),items.end(),[&](const auto& item) { return item.id==id; })) current=id;
    if (reveal) for (int i=0;i<items.size();++i) if (items[i].id==id) {
        const auto r=tile(i);
        if (r.top()<0) verticalScrollBar()->setValue(verticalScrollBar()->value()+r.top()-Gap);
        else if (r.bottom()>viewport()->height()) verticalScrollBar()->setValue(verticalScrollBar()->value()+r.bottom()-viewport()->height()+Gap);
        break;
    }
    if(previous!=current) { updateTile(previous); updateTile(current); }
}
void OverviewGrid::updateTile(const QString& id) {
    for(int i=0;i<items.size();++i)if(items[i].id==id) { viewport()->update(tile(i)); break; }
}
void OverviewGrid::reflow() {
    tiles.clear(); headings.clear();
    const int cols=columns(), width=(viewport()->width()-Gap*(cols+1))/cols;
    int y=Gap,col=0; QString previous; bool first=true;
    for (const auto& item:items) {
        if (grouped && (first || item.project!=previous)) {
            if (col) { y+=TileHeight+Gap; col=0; }
            headings.append({item.project,item.projectLabel,QRect(Gap,y,viewport()->width()-2*Gap,28)}); y+=34;
        }
        tiles.append(QRect(Gap+col*(width+Gap),y,width,TileHeight));
        if (++col==cols) { col=0; y+=TileHeight+Gap; }
        previous=item.project; first=false;
    }
    if (col) y+=TileHeight+Gap;
    verticalScrollBar()->setPageStep(viewport()->height()); verticalScrollBar()->setSingleStep(TileHeight+Gap);
    verticalScrollBar()->setRange(0,std::max(0,y-viewport()->height()));
}
QStringList OverviewGrid::visibleIds() const {
    QStringList ids;
    for (int i=0;i<items.size();++i) if (tile(i).intersects(viewport()->rect())) ids.append(items[i].id);
    return ids;
}
void OverviewGrid::paintEvent(QPaintEvent* event) {
    QPainter p(viewport()); p.setRenderHint(QPainter::Antialiasing);
    const auto pal=palette(); p.fillRect(viewport()->rect(),pal.brush(QPalette::Base));
    if (items.isEmpty()) {
        p.setPen(pal.color(QPalette::PlaceholderText)); p.drawText(viewport()->rect(),Qt::AlignCenter,"No open previews\nOpen or drop assets to build your overview."); return;
    }
    for (const auto& heading:headings) {
        const auto r=heading.rect.translated(0,-verticalScrollBar()->value()); if (!event->region().intersects(r)) continue;
        projectIcon(heading.id).paint(&p,QRect(r.left(),r.top()+3,20,20));
        p.setPen(pal.color(QPalette::Text)); p.drawText(r.adjusted(27,0,0,0),Qt::AlignVCenter,p.fontMetrics().elidedText(heading.label,Qt::ElideRight,r.width()-30));
        p.setPen(pal.color(QPalette::Mid)); p.drawLine(r.bottomLeft(),r.bottomRight());
    }
    for (int i=0;i<items.size();++i) {
        const auto r=tile(i); if (!event->region().intersects(r)) continue;
        const auto& item=items[i]; const bool active=item.id==current;
        p.setBrush(pal.brush(QPalette::AlternateBase));
        p.setPen(QPen(active?pal.color(QPalette::Highlight):pal.color(QPalette::Mid),active?2:1));
        p.drawRoundedRect(r.adjusted(1,1,-1,-1),5,5);
        const auto imageArea=r.adjusted(6,6,-6,-40);
        const auto image=cache->peekThumbnail(keys.value(item.id));
        if (!image.isNull()) {
            const auto size=image.size().scaled(imageArea.size(),Qt::KeepAspectRatio);
            p.setRenderHint(QPainter::SmoothPixmapTransform); p.drawImage(QRect(imageArea.center()-QPoint(size.width()/2,size.height()/2),size),image);
        } else {
            p.setPen(pal.color(QPalette::PlaceholderText)); auto font=p.font(); font.setBold(true); p.setFont(font);
            p.drawText(imageArea,Qt::AlignCenter,item.kind=="image"?"Image":item.kind=="model"?"3D":item.kind=="material"?"Material":item.kind=="audio"?"Audio":item.kind=="video"?"Video":"Graph");
            if(item.id==loadingId) {
                p.setPen(QPen(pal.color(QPalette::Highlight),2,Qt::SolidLine,Qt::RoundCap));
                p.drawArc(QRectF(imageArea.right()-20,imageArea.bottom()-20,14,14),-spinAngle*16,250*16);
            }
        }
        auto font=p.font(); font.setBold(false); p.setFont(font); p.setPen(pal.color(QPalette::Text));
        p.drawText(r.adjusted(9,r.height()-36,-9,-17),Qt::AlignLeft|Qt::AlignVCenter,p.fontMetrics().elidedText(item.label,Qt::ElideMiddle,r.width()-18));
        p.setPen(pal.color(QPalette::PlaceholderText)); font.setPointSizeF(std::max(8.0,font.pointSizeF()-1)); p.setFont(font);
        p.drawText(r.adjusted(9,r.height()-19,-9,-3),Qt::AlignLeft|Qt::AlignVCenter,p.fontMetrics().elidedText(item.projectLabel+" · "+item.kind.toUpper(),Qt::ElideRight,r.width()-18));
        p.setFont(viewport()->font());
        if (i==hover) { p.setBrush(pal.color(QPalette::Highlight).lighter(120)); p.setOpacity(.08); p.setPen(Qt::NoPen); p.drawRoundedRect(r.adjusted(2,2,-2,-2),4,4); p.setOpacity(1); }
        const auto box=checkbox(i);
        p.fillRect(box,pal.brush(QPalette::Window));
        QStyleOptionButton option; option.initFrom(this); option.rect=box.adjusted(5,5,-5,-5);
        option.state|=checked.contains(item.id)?QStyle::State_On:QStyle::State_Off;
        style()->drawPrimitive(QStyle::PE_IndicatorCheckBox,&option,&p,this);
    }
}
void OverviewGrid::schedule(int delay) {
    if (!isVisible() || window()->isMinimized()) { dispatch.stop(); return; }
    // Viewport activity replaces an earlier continuation, including while a
    // worker is busy. Its completion must respect the remaining quiet period.
    // A timeout during that worker simply settles; completion schedules anew.
    if(delay==25 || !dispatch.isActive())dispatch.start(delay);
}
void OverviewGrid::work() {
    if (busy || !isVisible() || window()->isMinimized()) return;
    for (int i=0;i<items.size();++i) {
        if (!tile(i).intersects(viewport()->rect())) continue;
        const auto item=items[i];
        if(!keys.contains(item.id)) { keys[item.id]=keyFor?keyFor(item.id):QString(); viewport()->update(tile(i)); }
        const auto key=keys.value(item.id);
        if (key.isEmpty() || attempted.value(item.id)==key) continue;
        attempted.insert(item.id,key);
        if (!cache->thumbnail(key).isNull()) { viewport()->update(tile(i)); continue; }
        if (item.kind=="model" || item.kind=="material") {
            if (cache->allowsPreload()) render(item,key);
            if (busy) return;
            continue;
        }
        if (item.kind!="image") continue;
        busy=true; loadingId=item.id; spinner.start(); viewport()->update(tile(i));
        const int token=epoch; QPointer<OverviewGrid> guard(this);
        const auto source=preview::sourceCacheKey("thumbnail-source-v1",item.path);
        worker.start(QRunnable::create([guard,item,key,source,token] {
            QElapsedTimer timer; timer.start();
            auto image=smallImage(item.path);
            if (source!=preview::sourceCacheKey("thumbnail-source-v1",item.path)) image={};
            const auto elapsedUs=timer.nsecsElapsed()/1000;
            if (!guard) return;
            QMetaObject::invokeMethod(guard,[guard,item,key,token,elapsedUs,image=std::move(image)]() mutable {
                if (!guard) return;
                guard->busy=false;
                guard->loadingId.clear(); guard->spinner.stop();
                const bool publish=token==guard->epoch && guard->isVisible() && !guard->window()->isMinimized() &&
                    guard->visibleIds().contains(item.id) && guard->keyFor && guard->keyFor(item.id)==key && !image.isNull();
                if (!publish && !image.isNull()) guard->attempted.remove(item.id);
                guard->cache->observe("thumbnail",publish?"generated":image.isNull()?"skipped":"discarded",key,image.sizeInBytes(),
                    publish?"visible":image.isNull()?"bounded_or_failed":"hidden_or_replaced",elapsedUs);
                if (publish)
                    guard->cache->putThumbnail(key,std::move(image));
                // Yield to input between jobs without debouncing an already
                // settled viewport again for every completed thumbnail.
                guard->updateTile(item.id); guard->schedule(publish?1:25);
            },Qt::QueuedConnection);
        }));
        return;
    }
}
void OverviewGrid::render(const Item& item,const QString& key) {
    const auto config=configFor?configFor(item.id):QJsonObject{};
    const QFileInfo source(item.path);
    const auto request=QJsonDocument(config).toJson(QJsonDocument::Compact);
    if (config.isEmpty() || renderRoot.isEmpty() || !source.isFile() || source.size()>256*1024*1024 || request.size()>64*1024) return;
    renderDirectory=std::make_unique<QTemporaryDir>(renderRoot+"/job-XXXXXX");
    if (!renderDirectory->isValid()) { renderDirectory.reset(); return; }
    QFile input(renderDirectory->filePath("request.json"));
    if (!input.open(QIODevice::WriteOnly) || input.write(request)!=request.size()) { renderDirectory.reset(); return; }
    input.close();
    busy=true; renderCancelled=false; renderTimedOut=false; renderingId=item.id; renderingKey=key;
    loadingId=item.id; spinner.start(); updateTile(item.id);
    const int token=epoch;
    auto* process=new QProcess(this); renderWorker=process;
    process->setChildProcessModifier([owner=::getpid()] {
        if (::setsid()<0 || ::prctl(PR_SET_PDEATHSIG,SIGKILL)<0 || ::getppid()!=owner) _exit(126);
    });
    auto environment=QProcessEnvironment::systemEnvironment();
    environment.insert("TMPDIR",renderDirectory->path());
    process->setProcessEnvironment(environment);
    process->setStandardOutputFile(QProcess::nullDevice()); process->setStandardErrorFile(QProcess::nullDevice());
    auto elapsed=std::make_shared<QElapsedTimer>(); elapsed->start();
    auto finish=[this,process,item,key,token,elapsed](bool success) {
        if (renderWorker!=process) return;
        renderDeadline->stop();
        QFile receipt(renderDirectory->filePath("result.json"));
        QJsonObject details;
        if (receipt.open(QIODevice::ReadOnly) && receipt.size()<=4096) details=QJsonDocument::fromJson(receipt.readAll()).object();
        QImage image;
        const auto path=renderDirectory->filePath("thumbnail.png");
        if (success && QFileInfo(path).size()<=256*1024) image=smallImage(path);
        const bool publish=!renderCancelled && token==epoch && isVisible() && !window()->isMinimized() &&
            visibleIds().contains(item.id) && keyFor && keyFor(item.id)==key && !image.isNull();
        if ((renderCancelled && !renderTimedOut) || (!publish && !image.isNull())) attempted.remove(item.id);
        if (!details["renderer"].toString().isEmpty()) lastRenderer=details["renderer"].toString();
        lastRenderError=details["error"].toString();
        if (!success && !renderCancelled && lastRenderError.isEmpty()) lastRenderError="Thumbnail worker failed";
        if (renderTimedOut) lastRenderError="Thumbnail timed out";
        cache->observe("thumbnail",publish?"generated":image.isNull()?"skipped":"discarded",key,image.sizeInBytes(),
            publish?"visible_model":renderTimedOut?"timeout":renderCancelled?"cancelled":image.isNull()?"bounded_or_failed":"hidden_or_replaced",elapsed->nsecsElapsed()/1000,details);
        if (publish) { cache->putThumbnail(key,std::move(image)); ++rendered; }
        renderWorker.clear(); renderDirectory.reset(); renderingId.clear(); renderingKey.clear(); busy=false;
        loadingId.clear(); spinner.stop();
        process->deleteLater(); updateTile(item.id); schedule(publish?1:25);
    };
    connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[finish](int code,QProcess::ExitStatus status) {
        finish(code==0 && status==QProcess::NormalExit);
    });
    connect(process,&QProcess::errorOccurred,this,[finish](QProcess::ProcessError error) { if (error==QProcess::FailedToStart) finish(false); });
    renderDeadline->start(30000);
    process->start(QCoreApplication::applicationFilePath(),{"--thumbnail-job",renderDirectory->path()});
    if (viewportChanged) viewportChanged();
}
void OverviewGrid::cancelRender() {
    if (!renderWorker) return;
    renderCancelled=true; renderDeadline->stop();
    const auto pid=renderWorker->processId();
    if (pid>0) ::kill(-pid,SIGKILL);
    renderWorker->kill();
}
void OverviewGrid::invalidate() { keys.clear(); attempted.clear(); ++epoch; cancelRender(); viewport()->update(); schedule(); }
void OverviewGrid::refresh() {
    keys.clear();
    // Directory notifications can be unrelated to any registered source. Do
    // not restart an unchanged import or retry a failed identity on each one.
    if (renderWorker && keyFor && keyFor(renderingId)!=renderingKey) cancelRender();
    viewport()->update(); schedule();
}
void OverviewGrid::resizeEvent(QResizeEvent* e) {
    keys.clear();
    QAbstractScrollArea::resizeEvent(e); reflow();
    if (!renderingId.isEmpty() && !visibleIds().contains(renderingId)) cancelRender();
    schedule(); if (viewportChanged) viewportChanged();
}
void OverviewGrid::showEvent(QShowEvent* e) { QAbstractScrollArea::showEvent(e); reflow(); schedule(); }
void OverviewGrid::hideEvent(QHideEvent* e) { ++epoch; keys.clear(); attempted.clear(); dispatch.stop(); spinner.stop(); cancelRender(); QAbstractScrollArea::hideEvent(e); }
void OverviewGrid::mousePressEvent(QMouseEvent* e) {
    if (e->button()!=Qt::LeftButton) return;
    setFocus(Qt::MouseFocusReason);
    for (int i=0;i<items.size();++i) if (tile(i).contains(e->position().toPoint())) {
        const auto id=items[i].id;
        if (checkbox(i).contains(e->position().toPoint()) || (e->modifiers() & (Qt::ControlModifier|Qt::ShiftModifier))) {
            if (marked) marked(id,e->modifiers());
            return;
        }
        // Defer navigation until the event receiver has finished dispatching.
        QTimer::singleShot(0,this,[this,id] { if (activated) activated(id); }); return;
    }
}
void OverviewGrid::mouseMoveEvent(QMouseEvent* e) {
    int next=-1; for (int i=0;i<items.size();++i) if (tile(i).contains(e->position().toPoint())) { next=i; break; }
    if (hover!=next) { const auto previous=hover; hover=next; if(previous>=0)viewport()->update(tile(previous)); if(hover>=0)viewport()->update(tile(hover)); }
}
bool OverviewGrid::viewportEvent(QEvent* event) {
    if (event->type()==QEvent::ToolTip) {
        if (hover>=0 && hover<items.size()) QToolTip::showText(QCursor::pos(),items[hover].label+"\n"+items[hover].path+"\nClick to open · Checkbox / Ctrl+click to select · Shift+click for a range",viewport());
        return true;
    }
    if (event->type()==QEvent::Leave) { const auto previous=hover; hover=-1; if(previous>=0)viewport()->update(tile(previous)); }
    return QAbstractScrollArea::viewportEvent(event);
}
void OverviewGrid::keyPressEvent(QKeyEvent* e) {
    int at=0; for (int i=0;i<items.size();++i) if (items[i].id==current) { at=i; break; }
    int next=at;
    if (e->key()==Qt::Key_Space && (e->modifiers() & Qt::ControlModifier)) {
        if (!current.isEmpty() && marked) marked(current,e->modifiers());
        return;
    }
    if (e->key()==Qt::Key_Return || e->key()==Qt::Key_Enter || e->key()==Qt::Key_Space) { if (!current.isEmpty() && activated) activated(current); return; }
    if (e->key()==Qt::Key_Left) --next;
    else if (e->key()==Qt::Key_Right) ++next;
    else if (e->key()==Qt::Key_Up) next-=columns();
    else if (e->key()==Qt::Key_Down) next+=columns();
    else if (e->key()==Qt::Key_Home) next=0;
    else if (e->key()==Qt::Key_End) next=items.size()-1;
    else { QAbstractScrollArea::keyPressEvent(e); return; }
    if (items.isEmpty()) return;
    next=std::clamp(next,0,int(items.size()-1));
    select(items[next].id,true); if (selected) selected(items[next].id);
}
QJsonObject OverviewGrid::describe(QWidget* window) const {
    QJsonArray visible;
    for (int i=0;i<items.size();++i) if (tile(i).intersects(viewport()->rect())) {
        const auto r=tile(i); const auto origin=viewport()->mapTo(window,r.topLeft());
        const auto box=checkbox(i); const auto boxOrigin=viewport()->mapTo(window,box.topLeft());
        visible.append(QJsonObject{{"id",items[i].id},{"project",items[i].project},{"x",origin.x()},{"y",origin.y()},{"width",r.width()},{"height",r.height()},
            {"checked",checked.contains(items[i].id)},
            {"checkbox",QJsonObject{{"x",boxOrigin.x()},{"y",boxOrigin.y()},{"width",box.width()},{"height",box.height()}}}});
    }
    QJsonArray groups; for (const auto& heading:headings) groups.append(QJsonObject{{"id",heading.id},{"label",heading.label}});
    return {{"visible",isVisible()},{"grouped",grouped},{"groups",groups},{"items",items.size()},{"columns",columns()},{"tiles",visible},
        {"worker_active",busy || dispatch.isActive()},{"worker_limit",1},{"thumbnail_width",256},{"thumbnail_height",160},
        {"loading_id",loadingId},{"spinner_active",spinner.isActive()},
        {"worker_id",renderingId},{"worker_pid",renderWorker?double(renderWorker->processId()):0},
        {"worker_nice",19},{"rendered",rendered},{"renderer",lastRenderer},{"render_error",lastRenderError}};
}
