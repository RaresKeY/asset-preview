#include "overview_grid.h"
#include "thumbnail_queue.h"
#include "project_identity.h"
#include <QHash>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QStyle>
#include <QStyleOptionButton>
#include <QToolTip>
#include <QTimer>
#include <algorithm>

namespace { constexpr int Gap=12,TileWidth=172,TileHeight=146; }
OverviewGrid::OverviewGrid(preview::PreviewCache* store,const QString& jobDirectory,QWidget* parent):QAbstractScrollArea(parent),cache(store),queue(new ThumbnailQueue(store,jobDirectory,this)) {
    setObjectName("overviewGrid"); setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::StrongFocus); setAccessibleName("All open previews");
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent); viewport()->setMouseTracking(true);
    queue->keyFor=[this](const QString& id) { return keyFor?keyFor(id):QString(); };
    queue->configFor=[this](const QString& id) { return configFor?configFor(id):QJsonObject{}; };
    queue->changed=[this](const QString& id,const QString& key) {
        if(!isVisible())return;
        for(int i=0;i<items.size();++i)if(items[i].id==id) {
            const auto r=tile(i);
            if(r.intersects(viewport()->rect())) { keys[id]=key; updateTile(id); }
            break;
        }
    };
    queue->activityChanged=[this] { thumbnailState(); };
    queue->sourcesChanged=[this] { if(viewportChanged)viewportChanged(); };
    spinner.setInterval(80);
    connect(&spinner,&QTimer::timeout,this,[this] {
        spinAngle=(spinAngle+30)%360;
        for(const auto& id:loading)updateTile(id);
    });
    connect(verticalScrollBar(),&QScrollBar::valueChanged,this,[this] {
        if(reflowing)return;
        updateHover(hoverPosition); pruneKeys(); viewport()->update(); syncDemand(); if(viewportChanged)viewportChanged();
    });
}
OverviewGrid::~OverviewGrid() {
    queue->changed={}; queue->activityChanged={}; queue->sourcesChanged={}; delete queue;
}
int OverviewGrid::columns() const { return layoutColumns; }
QRect OverviewGrid::tile(int i) const {
    return tiles.value(i).translated(0,-verticalScrollBar()->value());
}
QRect OverviewGrid::paintArea(int i) const {
    // Antialiased strokes can touch physical pixels outside the logical tile,
    // especially at fractional scale. Every partial invalidation must cover them.
    return tile(i).adjusted(-2,-2,2,2);
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
    if (changed) {
        QSet<QString> surviving;
        for(const auto& item:next)surviving.insert(item.id);
        const auto anchor=scrollAnchor(surviving);
        QHash<QString,int> previousIndex; previousIndex.reserve(items.size());
        for (int i=0;i<items.size();++i) previousIndex.insert(items[i].id,i);
        QMap<QString,QString> retained;
        for (const auto& item:next) {
            const auto it=previousIndex.constFind(item.id);
            if (it!=previousIndex.cend()) {
                const auto& previous=items[*it];
                if (item.path==previous.path && item.kind==previous.kind && keys.contains(item.id))
                    retained.insert(item.id,keys.value(item.id));
            }
        }
        items=std::move(next); keys=std::move(retained); reflow(anchor);
    }
    QString target=id;
    if (std::none_of(items.begin(),items.end(),[&](const auto& item) { return item.id==target; })) target=items.isEmpty()?QString():items.first().id;
    if(!hasWarmItems) {
        QList<ThumbnailQueue::Item> targets;
        for(const auto& item:items)targets.append({item.id,item.path,item.kind});
        queue->setItems(targets);
    }
    select(target); syncDemand();
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
    for(int i=0;i<items.size();++i)if(items[i].id==id) { viewport()->update(paintArea(i)); break; }
}
OverviewGrid::ScrollAnchor OverviewGrid::scrollAnchor(const QSet<QString>& surviving) const {
    for(int i=0;i<items.size() && i<tiles.size();++i)
        if((surviving.isEmpty() || surviving.contains(items[i].id)) && tile(i).intersects(viewport()->rect()))
            return {items[i].id,tile(i).top()};
    return {};
}
void OverviewGrid::reflow(const ScrollAnchor& anchor) {
    if(reflowing)return;
    const QScopedValueRollback guard(reflowing,true);
    QSize extent;
    do {
        extent=viewport()->size();
        const int cols=std::max(1,(extent.width()-Gap)/(TileWidth+Gap));
        const int available=std::max(cols,extent.width()-Gap*(cols+1));
        QList<QRect> nextTiles; QList<Heading> nextHeadings;
        nextTiles.reserve(items.size());
        int y=Gap,col=0; QString previous; bool first=true;
        for(const auto& item:items) {
            if(grouped && (first || item.project!=previous)) {
                if(col) { y+=TileHeight+Gap; col=0; }
                nextHeadings.append({item.project,item.projectLabel,QRect(Gap,y,std::max(1,extent.width()-2*Gap),28)}); y+=34;
            }
            // Divide edges, so rounding remainder is distributed across columns.
            const int left=Gap+col*Gap+col*available/cols;
            const int right=Gap+col*Gap+(col+1)*available/cols;
            nextTiles.append(QRect(left,y,right-left,TileHeight));
            if(++col==cols) { col=0; y+=TileHeight+Gap; }
            previous=item.project; first=false;
        }
        if(col)y+=TileHeight+Gap;
        tiles=std::move(nextTiles); headings=std::move(nextHeadings); layoutColumns=cols;
        verticalScrollBar()->setPageStep(extent.height());
        verticalScrollBar()->setSingleStep(std::max(16,fontMetrics().height()+Gap));
        verticalScrollBar()->setRange(0,std::max(0,y-extent.height()));
        // Showing/hiding the vertical bar can synchronously resize the viewport.
        // Commit one consistent geometry before publishing demand or painting.
    } while(extent!=viewport()->size());
    if(!anchor.id.isEmpty())for(int i=0;i<items.size();++i)if(items[i].id==anchor.id) {
        verticalScrollBar()->setValue(tiles[i].top()-anchor.top); break;
    }
    updateHover(hoverPosition);
    pruneKeys();
    // Reflow can move every tile/header, including inside an existing viewport.
    viewport()->update();
}
QStringList OverviewGrid::visibleIds() const {
    QStringList ids;
    for (int i=0;i<items.size();++i) if (tile(i).intersects(viewport()->rect())) ids.append(items[i].id);
    return ids;
}
void OverviewGrid::pruneKeys() {
    // Sources are watched only while visible; validate them anew on return.
    const auto visibleList=visibleIds();
    const QSet<QString> visible(visibleList.begin(),visibleList.end());
    for (auto it=keys.begin();it!=keys.end();) {
        if (!visible.contains(it.key())) it=keys.erase(it); else ++it;
    }
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
        const auto r=tile(i); if (!event->region().intersects(paintArea(i))) continue;
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
            if(loading.contains(item.id)) {
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
        // These are painted cells, not one shared checkbox widget. Keep hover
        // and style animation state local to the cell under the pointer.
        option.state&=~(QStyle::State_MouseOver|QStyle::State_HasFocus|QStyle::State_Sunken);
        if(hover==i && box.contains(hoverPosition))option.state|=QStyle::State_MouseOver;
        option.styleObject=nullptr;
        option.state|=checked.contains(item.id)?QStyle::State_On:QStyle::State_Off;
        style()->drawPrimitive(QStyle::PE_IndicatorCheckBox,&option,&p,nullptr);
    }
}
void OverviewGrid::warmItems(QList<Item> all,QStringList next,QSet<QString> shown,bool ready,bool enabled) {
    hasWarmItems=true; nearby=std::move(next); active=std::move(shown); warmReady=ready; warmEnabled=enabled;
    QList<ThumbnailQueue::Item> targets; targets.reserve(all.size());
    for(const auto& item:all)targets.append({item.id,item.path,item.kind});
    queue->setItems(targets); syncDemand();
}
QStringList OverviewGrid::workingIds() const { return queue->workingIds(); }
bool OverviewGrid::backgroundAllowed() const { return queue->backgroundAllowed(); }
void OverviewGrid::syncDemand() {
    const bool enabled=window()->isVisible() && !window()->isMinimized();
    queue->demand(isVisible()?visibleIds():QStringList{},nearby,active,
        enabled && (isVisible() || (hasWarmItems && warmEnabled)),hasWarmItems && warmReady && warmEnabled);
    thumbnailState();
}
void OverviewGrid::thumbnailState() {
    const auto previous=loading; loading=queue->loadingIds();
    const auto visibleList=visibleIds();
    const QSet<QString> visible(visibleList.begin(),visibleList.end());
    bool animate=false;
    for(const auto& id:loading)if(isVisible() && visible.contains(id)) { animate=true; break; }
    if(animate && !spinner.isActive())spinner.start(); else if(!animate)spinner.stop();
    for(const auto& id:previous-loading)updateTile(id);
    for(const auto& id:loading-previous)updateTile(id);
}
void OverviewGrid::invalidate() { keys.clear(); queue->refresh(true); viewport()->update(); syncDemand(); }
void OverviewGrid::refresh() {
    if(isVisible())for(const auto& id:visibleIds())keys[id]=keyFor?keyFor(id):QString();
    pruneKeys(); queue->refresh(); viewport()->update(); syncDemand();
}
void OverviewGrid::resizeEvent(QResizeEvent* e) {
    QAbstractScrollArea::resizeEvent(e);
    if(reflowing)return;
    reflow(scrollAnchor()); syncDemand(); if(viewportChanged)viewportChanged();
}
void OverviewGrid::showEvent(QShowEvent* e) { QAbstractScrollArea::showEvent(e); reflow(scrollAnchor()); syncDemand(); }
void OverviewGrid::hideEvent(QHideEvent* e) { keys.clear(); hover=-1; hoverPosition={-1,-1}; spinner.stop(); QAbstractScrollArea::hideEvent(e); syncDemand(); }
void OverviewGrid::scrollContentsBy(int,int) {
    // Repaint from the logical scroll origin instead of shifting rounded device
    // pixels, which can drift relative to later partial paints at fractional DPR.
    viewport()->update();
}
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
    updateHover(e->position().toPoint());
}
void OverviewGrid::updateHover(QPoint position) {
    const bool wasOverCheckbox=hover>=0 && checkbox(hover).contains(hoverPosition);
    hoverPosition=position;
    int next=-1;
    if(viewport()->rect().contains(position))for(int i=0;i<items.size();++i)if(tile(i).contains(position)) { next=i; break; }
    if(hover!=next) { const auto previous=hover; hover=next; if(previous>=0)viewport()->update(paintArea(previous)); if(hover>=0)viewport()->update(paintArea(hover)); }
    else if(hover>=0 && wasOverCheckbox!=checkbox(hover).contains(position))viewport()->update(paintArea(hover));
}
bool OverviewGrid::viewportEvent(QEvent* event) {
    if (event->type()==QEvent::ToolTip) {
        if (hover>=0 && hover<items.size()) QToolTip::showText(QCursor::pos(),items[hover].label+"\n"+items[hover].path+"\nClick to open · Checkbox / Ctrl+click to select · Shift+click for a range",viewport());
        return true;
    }
    if (event->type()==QEvent::Leave)updateHover({-1,-1});
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
    auto result=queue->metrics();
    result["visible"]=isVisible(); result["grouped"]=grouped; result["groups"]=groups;
    result["native"]=bool(internalWinId());
    int nativeAncestors=0;
    for(auto* parent=parentWidget();parent && !parent->isWindow();parent=parent->parentWidget())
        if(parent->internalWinId())++nativeAncestors;
    result["native_ancestors"]=nativeAncestors;
    result["items"]=items.size(); result["columns"]=columns(); result["tiles"]=visible;
    const auto origin=viewport()->mapTo(window,QPoint());
    result["viewport"]=QJsonObject{{"x",origin.x()},{"y",origin.y()},{"width",viewport()->width()},{"height",viewport()->height()}};
    result["scroll"]=verticalScrollBar()->value(); result["scroll_max"]=verticalScrollBar()->maximum();
    result["loading_id"]=loading.isEmpty()?QString():*loading.begin(); result["spinner_active"]=spinner.isActive();
    return result;
}
