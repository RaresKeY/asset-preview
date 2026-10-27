#include "project_grid.h"
#include "loading_label.h"
#include "project_identity.h"
#include <QComboBox>
#include <QCursor>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

ProjectGrid::ProjectGrid(QWidget* parent):QScrollArea(parent),content(new QWidget) {
    emptyState=new QLabel("No projects to show\nAgents can create a project or send a preview to get started.",content);
    emptyState->setAlignment(Qt::AlignCenter); emptyState->setWordWrap(true);
    setObjectName("projectGrid"); setFrameShape(QFrame::NoFrame); setWidget(content);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setAccessibleName("Project previews"); setAttribute(Qt::WA_Hover);
    setStyleSheet("QScrollBar:vertical {width:8px; background:transparent; margin:0;}"
        "QScrollBar::handle:vertical {background:transparent; min-height:24px; margin:0 2px; border-radius:2px;}"
        "QScrollArea:hover QScrollBar::handle:vertical, QScrollBar:focus::handle:vertical, QScrollBar::handle:vertical:hover {background:#73777a;}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {height:0;}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {background:transparent;}");
    connect(verticalScrollBar(),&QScrollBar::valueChanged,this,[this] { schedule(); });
}
void ProjectGrid::detach(Slot& slot) {
    if (!slot.card) return;
    slot.body->removeWidget(slot.card); slot.card->hide(); slot.card->setParent(parentWidget()); slot.card.clear();
}
void ProjectGrid::setProjects(const QJsonArray& items,int requested) {
    QStringList next;
    for (const auto& value:items) next.append(value.toObject()["id"].toString());
    for (auto it=panesById.begin();it!=panesById.end();) {
        if (next.contains(it.key())) { ++it; continue; }
        detach(it.value()); delete it->pane; it=panesById.erase(it);
    }
    order=next; density=requested;
    for (const auto& value:items) {
        const auto item=value.toObject(); const auto id=item["id"].toString();
        if (!panesById.contains(id)) {
            Slot slot; slot.pane=new QFrame(content); slot.pane->setObjectName("projectPane");
            slot.body=new QVBoxLayout(slot.pane); slot.body->setContentsMargins(1,1,1,1); slot.body->setSpacing(0);
            auto* bar=new QWidget(slot.pane); auto* line=new QHBoxLayout(bar); line->setContentsMargins(5,2,5,2); line->setSpacing(5);
            slot.mark=new QLabel(bar); slot.mark->setPixmap(projectIcon(id).pixmap(20,20)); slot.mark->setFixedSize(20,20);
            slot.label=new QLabel(bar); slot.label->setTextFormat(Qt::PlainText); slot.label->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
            slot.choices=new QComboBox(bar); slot.choices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            slot.choices->setMinimumContentsLength(2); slot.choices->setMinimumWidth(50);
            slot.choices->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
            line->addWidget(slot.mark); line->addWidget(slot.label,1); line->addWidget(slot.choices,1);
            slot.body->addWidget(bar);
            slot.empty=new LoadingLabel(slot.pane); slot.empty->setText("Waiting for a preview"); slot.empty->setAlignment(Qt::AlignCenter); slot.empty->setWordWrap(true);
            slot.body->addWidget(slot.empty,1); panesById.insert(id,slot);
            connect(slot.choices,&QComboBox::activated,this,[this,id](int index) {
                const auto current=panesById[id].choices->itemData(index).toString();
                // A current change may destroy the sender's surrounding preview.
                QTimer::singleShot(0,this,[this,id,current] { if(chosen) chosen(id,current); });
            });
        }
        auto& slot=panesById[id]; const auto current=item["current"].toString();
        if (slot.current!=current) detach(slot);
        slot.current=current; slot.label->setProperty("fullLabel",item["label"].toString()); slot.label->setText(item["label"].toString()); slot.label->setToolTip(item["label"].toString()+"\n"+id);
        slot.choices->setAccessibleName("Current preview for "+item["label"].toString());
        const QSignalBlocker blocker(slot.choices);
        const auto members=item["entries"].toArray();
        bool changed=slot.choices->count()!=members.size();
        if(!changed)for(int at=0;at<members.size();++at) {
            const auto member=members[at].toObject();
            if(slot.choices->itemData(at)!=member["id"] || slot.choices->itemText(at)!=member["label"].toString()) { changed=true; break; }
        }
        if(changed) {
            slot.choices->clear();
            for (const auto& asset:members) {
                const auto e=asset.toObject(); slot.choices->addItem(e["label"].toString(),e["id"].toString());
            }
        }
        slot.choices->setCurrentIndex(slot.choices->findData(current)); slot.choices->setEnabled(!id.isEmpty() && slot.choices->count()>0);
        slot.choices->setToolTip(id.isEmpty()?"Open individual ungrouped previews in Overview":slot.choices->currentText());
        slot.empty->setText(slot.current.isEmpty()?"Waiting for a preview":"Loading preview");
        slot.empty->setVisible(!slot.card); slot.pane->show();
    }
    reflow();
}
void ProjectGrid::reflow() {
    const int count=order.size(), width=std::max(1,viewport()->width()),height=std::max(1,viewport()->height());
    const int requested=density?density:std::max(1,int(std::ceil(std::sqrt(count))));
    columns=std::clamp(requested,1,std::max(1,width/220));
    rows=std::max(1,(count+columns-1)/columns);
    const int visibleRows=density?density:rows;
    const int cellHeight=std::max(170,(height-2*(visibleRows-1))/std::max(1,visibleRows));
    content->resize(width,std::max(height,rows*(cellHeight+2)-2));
    emptyState->setGeometry(content->rect()); emptyState->setVisible(count==0);
    for (int i=0;i<count;++i) {
        const int left=(i%columns)*width/columns,right=(i%columns+1)*width/columns;
        panesById[order[i]].pane->setGeometry(left,(i/columns)*(cellHeight+2),right-left-2,cellHeight);
        auto& slot=panesById[order[i]];
        slot.body->activate();
        if (auto* header=slot.label->parentWidget()->layout()) header->activate();
        slot.label->setText(slot.label->fontMetrics().elidedText(slot.label->property("fullLabel").toString(),Qt::ElideRight,slot.label->width()));
    }
    const QRect visible(content->mapFrom(viewport(),QPoint()),viewport()->size());
    for(auto& slot:panesById)
        static_cast<LoadingLabel*>(slot.empty)->setLoading(isVisible() && !slot.current.isEmpty() && !slot.card && slot.pane->geometry().intersects(visible));
}
QStringList ProjectGrid::visibleIds() const {
    QStringList ids;
    if (!isVisible()) return ids;
    const QRect visible(QPoint(0,verticalScrollBar()->value()),viewport()->size());
    for (const auto& id:order) {
        const auto& slot=panesById[id];
        if (!slot.current.isEmpty() && slot.pane->geometry().intersects(visible)) ids.append(slot.current);
    }
    return ids;
}
QString ProjectGrid::adjacentHovered(int delta) const {
    if (!isVisible()) return {};
    const auto position=viewport()->mapFromGlobal(QCursor::pos());
    if (!viewport()->rect().contains(position)) return {};
    const auto point=content->mapFrom(viewport(),position);
    for (const auto& id:order) {
        const auto& slot=panesById[id];
        if (id.isEmpty() || !slot.pane->geometry().contains(point)) continue;
        const int index=slot.choices->currentIndex(),target=index+delta;
        if (index<0 || target<0 || target>=slot.choices->count()) return {};
        return slot.choices->itemData(target).toString();
    }
    return {};
}
void ProjectGrid::attach(const QString& id,QWidget* card) {
    for (auto& slot:panesById) if (slot.current==id) {
        if (slot.card!=card || card->parentWidget()!=slot.pane) { detach(slot); slot.card=card; slot.body->addWidget(card,1); }
        slot.empty->hide(); card->show(); slot.body->activate(); return;
    }
}
QWidget* ProjectGrid::cardParent(const QString& id) const {
    for(const auto& slot:panesById)if(slot.current==id)return slot.pane;
    return nullptr;
}
void ProjectGrid::schedule() {
    if (pending) return;
    pending=true; QTimer::singleShot(0,this,[this] { pending=false; if (isVisible() && viewportChanged) viewportChanged(); });
}
void ProjectGrid::resizeEvent(QResizeEvent* e) { QScrollArea::resizeEvent(e); reflow(); schedule(); }
void ProjectGrid::showEvent(QShowEvent* e) { QScrollArea::showEvent(e); reflow(); schedule(); }
QJsonObject ProjectGrid::describe(QWidget* window) const {
    QJsonArray panes;
    for (const auto& id:order) {
        const auto& slot=panesById[id]; const auto p=slot.pane->mapTo(window,QPoint());
        const auto c=slot.choices->mapTo(window,QPoint());
        panes.append(QJsonObject{{"project",id},{"current",slot.current},{"x",p.x()},{"y",p.y()},
            {"loading",slot.empty->property("loading").toBool()},
            {"width",slot.pane->width()},{"height",slot.pane->height()},
            {"choice",QJsonObject{{"x",c.x()},{"y",c.y()},{"width",slot.choices->width()},{"height",slot.choices->height()}}}});
    }
    return {{"visible",isVisible()},{"columns",columns},{"rows",rows},{"panes",panes},{"scroll_max",verticalScrollBar()->maximum()},{"scroll",verticalScrollBar()->value()}};
}
