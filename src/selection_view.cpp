#include "selection_view.h"
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QScopedValueRollback>
#include <QSet>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

class SelectionImage final : public QWidget {
public:
    SelectionImage(QString path, QWidget* parent):QWidget(parent),path(std::move(path)) {
        setObjectName("selectionImage");
        setAccessibleName("Reference image");
        setFixedHeight(172);
    }
    bool ready() const { return !image.isNull(); }
    void setInViewport(bool inside) {
        if (inside==inViewport) return;
        inViewport=inside;
        image={};
        if (inside) {
            const QFileInfo file(path);
            if (file.isFile() && file.size()<=8*1024*1024) {
                const auto format=file.suffix().toLower().toLatin1();
                QImageReader reader(path,format=="jpg"?"jpeg":format);
                reader.setAutoTransform(true);
                const auto size=reader.size();
                if (size.isValid() && qint64(size.width())*size.height()<=16*1024*1024) {
                    if (size.width()>560 || size.height()>344)
                        reader.setScaledSize(size.scaled(560,344,Qt::KeepAspectRatio));
                    image=reader.read();
                }
            }
        }
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        if (image.isNull()) {
            painter.drawText(rect(),Qt::AlignLeft|Qt::AlignVCenter,"Image unavailable");
            return;
        }
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const auto size=image.size().scaled(QSize(std::min(280,width()),height()),Qt::KeepAspectRatio);
        painter.drawImage(QRect(QPoint(0,(height()-size.height())/2),size),image);
    }
private:
    QString path;
    QImage image;
    bool inViewport=false;
};

namespace {
class VariantStrip final : public QScrollArea {
public:
    explicit VariantStrip(QWidget* parent):QScrollArea(parent) {
        setObjectName("selectionVariantStrip");
        setFrameShape(QFrame::NoFrame);
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
        horizontalScrollBar()->setAccessibleName("Scroll variants");
    }
    QSize sizeHint() const override {
        const int height=widget() && widget()->layout()?widget()->layout()->sizeHint().height():0;
        return {0,height+style()->pixelMetric(QStyle::PM_ScrollBarExtent)};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    bool eventFilter(QObject* watched,QEvent* event) override {
        if (watched==widget() && (event->type()==QEvent::LayoutRequest || event->type()==QEvent::Resize))
            updateGeometry();
        return QScrollArea::eventFilter(watched,event);
    }
};
class DecisionButton final : public QToolButton {
public:
    DecisionButton(bool accept, QWidget* parent):QToolButton(parent),positive(accept) {
        setCheckable(true); setFixedSize(56,56); setFocusPolicy(Qt::StrongFocus);
        setToolTip(accept?"Accept this option · click again to clear":"Deny this option · click again to clear");
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const bool dark=palette().color(QPalette::Window).lightness()<128;
        const QColor semantic(positive?(dark?"#70d6a0":"#147d48"):(dark?"#f18187":"#c03543"));
        QColor top(dark?255:0, dark?255:0, dark?255:0, underMouse()?24:12);
        QColor bottom(dark?255:0, dark?255:0, dark?255:0, underMouse()?9:3);
        if (isChecked()) { top=semantic; top.setAlpha(45); bottom=semantic; bottom.setAlpha(16); }
        QLinearGradient glass(0,0,0,height()); glass.setColorAt(0,top); glass.setColorAt(1,bottom);
        p.setBrush(glass);
        QColor border=palette().color(QPalette::WindowText); border.setAlpha(dark?40:45);
        p.setPen(QPen(isChecked()?semantic:border,1));
        p.drawRoundedRect(QRectF(rect()).adjusted(1,1,-1,-1),3,3);
        if (hasFocus()) { p.setPen(QPen(palette().color(QPalette::Highlight),2)); p.setBrush(Qt::NoBrush); p.drawRect(rect().adjusted(3,3,-4,-4)); }
        p.setOpacity(isEnabled()?1:.35);
        p.setPen(QPen(semantic,isDown()?3.7:3.1,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        const QPointF center=QRectF(rect()).center();
        if (positive) p.drawPolyline(QPolygonF{center+QPointF(-11,0),center+QPointF(-3,8),center+QPointF(12,-9)});
        else { p.drawLine(center+QPointF(-9,-9),center+QPointF(9,9)); p.drawLine(center+QPointF(-9,9),center+QPointF(9,-9)); }
    }
private:
    bool positive;
};
class SelectionMarkdown final : public QTextBrowser {
public:
    SelectionMarkdown(const QString& source,QWidget* parent):QTextBrowser(parent) {
        setFrameShape(QFrame::NoFrame); setOpenLinks(false); setOpenExternalLinks(false);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        setTextInteractionFlags(Qt::TextBrowserInteraction);
        document()->setDocumentMargin(0);
        auto font=this->font(); font.setPixelSize(14); setFont(font);
        document()->setDefaultStyleSheet("p {margin:4px 0 7px;} h1,h2,h3 {margin:8px 0 5px;} li {margin:3px 0;} pre {white-space:pre-wrap;}");
        document()->setMarkdown(source,QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)|QTextDocument::MarkdownNoHTML);
        setStyleSheet("QTextBrowser {background:transparent;border:0;padding:0;}");
        adjustHeight();
    }
protected:
    QVariant loadResource(int,const QUrl&) override {
        // Selection text is portable and inert: no local/remote images, CSS,
        // HTML imports or fallback filename loading from a decision manifest.
        QImage placeholder(1,1,QImage::Format_ARGB32_Premultiplied); placeholder.fill(Qt::transparent); return placeholder;
    }
    void resizeEvent(QResizeEvent* e) override { QTextBrowser::resizeEvent(e); adjustHeight(); }
private:
    void adjustHeight() {
        document()->setTextWidth(std::max(40,viewport()->width()));
        const int wanted=std::clamp(int(std::ceil(document()->size().height()))+4,24,50000);
        if (height()!=wanted) setFixedHeight(wanted);
    }
};
QLabel* label(const QString& text,QWidget* parent,bool bold=false) {
    auto* l=new QLabel(text,parent); l->setTextFormat(Qt::PlainText); l->setWordWrap(true);
    auto f=l->font(); f.setPixelSize(bold?16:13); f.setBold(bold); l->setFont(f); return l;
}
}

SelectionView::SelectionView(QWidget* parent):QWidget(parent) {
    setObjectName("selectionView");
    auto* outer=new QVBoxLayout(this); outer->setContentsMargins(0,0,0,0); outer->setSpacing(0);
    auto* header=new QWidget(this); toolbar=new QHBoxLayout(header); toolbar->setContentsMargins(14,8,10,8); toolbar->setSpacing(8);
    title=label("Selection",header,true); title->setObjectName("selectionTitle"); title->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); toolbar->addWidget(title,1);
    mode=new QComboBox(header); mode->addItems({"Batch","One at a time"}); mode->setAccessibleName("Selection presentation"); mode->setObjectName("selectionMode"); toolbar->addWidget(mode);
    outer->addWidget(header);
    searchTools=new QWidget(this); searchbar=new QHBoxLayout(searchTools); searchbar->setContentsMargins(14,0,14,9); searchbar->setSpacing(8);
    filter=new QComboBox(searchTools); filter->addItems({"All options","Undecided","Accepted","Denied"}); filter->setAccessibleName("Filter decisions"); filter->setObjectName("selectionFilter"); searchbar->addWidget(filter);
    search=new QLineEdit(searchTools); search->setPlaceholderText("Find an option"); search->setClearButtonEnabled(true); search->setAccessibleName("Find an option"); searchbar->addWidget(search,1);
    previous=new QToolButton(searchTools); previous->setText("‹"); previous->setAccessibleName("Previous option"); previous->setFixedSize(32,30); searchbar->addWidget(previous);
    position=label("0 / 0",searchTools); position->setAlignment(Qt::AlignCenter); position->setMinimumWidth(65); searchbar->addWidget(position);
    next=new QToolButton(searchTools); next->setText("›"); next->setAccessibleName("Next option"); next->setFixedSize(32,30); searchbar->addWidget(next);
    outer->addWidget(searchTools);
    counts=label("0 accepted   ·   0 denied   ·   0 undecided",this); counts->setObjectName("selectionCounts"); counts->setContentsMargins(14,7,14,7); outer->addWidget(counts);
    notice=label({},this); notice->setObjectName("selectionNotice"); notice->setContentsMargins(14,6,14,6); notice->hide(); outer->addWidget(notice);
    scroll=new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content=new QWidget(scroll); content->setObjectName("selectionContent"); contents=new QVBoxLayout(content); contents->setContentsMargins(0,0,0,10); contents->setSpacing(0);
    scroll->setWidget(content); outer->addWidget(scroll,1);
    imageRefresh=new QTimer(this); imageRefresh->setSingleShot(true);
    connect(imageRefresh,&QTimer::timeout,this,&SelectionView::refreshImages);
    connect(scroll->verticalScrollBar(),&QScrollBar::valueChanged,this,[this] { imageRefresh->start(0); });
    empty=label("No options yet",content); empty->setContentsMargins(18,25,18,25); contents->addWidget(empty); contents->addStretch();
    connect(mode,&QComboBox::currentIndexChanged,this,[this] { refresh(); });
    connect(filter,&QComboBox::currentIndexChanged,this,[this] { current=0; refresh(); });
    connect(search,&QLineEdit::textChanged,this,[this] { current=0; refresh(); });
    connect(previous,&QToolButton::clicked,this,[this] { navigate(-1); }); connect(next,&QToolButton::clicked,this,[this] { navigate(1); });
    connect(&watcher,&QFileSystemWatcher::fileChanged,this,[this] { readResults(); });
    connect(&watcher,&QFileSystemWatcher::directoryChanged,this,[this] { readResults(); });
    theme();
}
void SelectionView::setCardWidgets(QWidget* actions,QWidget* status) {
    if (actions) { actions->setParent(this); toolbar->addWidget(actions); actions->show(); }
    if (status) { status->setParent(this); static_cast<QVBoxLayout*>(layout())->insertWidget(3,status); }
}
bool SelectionView::reload(const QJsonObject& config,QString& error) {
    Selection::Document nextDoc;
    if (!Selection::load(config["path"].toString(),nextDoc,error)) { sourceValid=false; writable=false; resultError=error; refresh(); return false; }
    sourceValid=true;
    document=std::move(nextDoc); resultPath=config["decisions"].toString(Selection::defaultDecisionsPath(document.path));
    title->setText(document.title); title->setToolTip(document.title);
    const int y=scroll->verticalScrollBar()->value();
    rebuild(); readResults(); ++loads;
    scroll->verticalScrollBar()->setValue(y); return true;
}
void SelectionView::rebuild() {
    rows.clear(); sections.clear();
    while (auto* child=contents->takeAt(0)) { delete child->widget(); delete child; }
    auto* parent=scroll->widget();
    if (!document.markdown.isEmpty()) {
        auto* intro=new QWidget(parent); auto* box=new QVBoxLayout(intro); box->setContentsMargins(18,10,18,10);
        box->addWidget(new SelectionMarkdown(document.markdown,intro)); contents->addWidget(intro);
    }
    QString section;
    VariantStrip* strip=nullptr;
    QHBoxLayout* variants=nullptr;
    for (int i=0;i<document.items.size();++i) {
        const auto& item=document.items[i];
        if (section!=item.section) {
            section=item.section;
            auto* head=new QWidget(parent); head->setObjectName("selectionSection"); head->setProperty("sectionId",item.section);
            auto* box=new QVBoxLayout(head); box->setContentsMargins(18,16,18,10); box->setSpacing(4);
            auto* title=label(item.sectionTitle,head,true);
            auto* sectionBar=new QHBoxLayout;
            sectionBar->addWidget(title,1);
            box->addLayout(sectionBar);
            if (!item.sectionMarkdown.isEmpty()) box->addWidget(new SelectionMarkdown(item.sectionMarkdown,head));
            sections.append(head); contents->addWidget(head);
            if (document.variantRows) {
                strip=new VariantStrip(head);
                strip->setAccessibleName(item.sectionTitle+" variants");
                auto* cards=new QWidget(strip);
                variants=new QHBoxLayout(cards);
                variants->setContentsMargins(0,4,0,2);
                variants->setSpacing(0);
                variants->setAlignment(Qt::AlignLeft);
                strip->setWidget(cards); box->addWidget(strip);
                auto* bar=strip->horizontalScrollBar();
                auto* left=new QToolButton(head);
                auto* right=new QToolButton(head);
                left->setText("‹"); right->setText("›");
                left->setAccessibleName("Scroll "+item.sectionTitle+" variants left");
                right->setAccessibleName("Scroll "+item.sectionTitle+" variants right");
                left->setFixedSize(28,26); right->setFixedSize(28,26);
                sectionBar->addWidget(left); sectionBar->addWidget(right);
                connect(left,&QToolButton::clicked,bar,[bar] { bar->triggerAction(QAbstractSlider::SliderPageStepSub); });
                connect(right,&QToolButton::clicked,bar,[bar] { bar->triggerAction(QAbstractSlider::SliderPageStepAdd); });
                auto updateArrows=[bar,left,right] {
                    const bool overflow=bar->maximum()>bar->minimum();
                    left->setVisible(overflow); right->setVisible(overflow);
                    left->setEnabled(bar->value()>bar->minimum());
                    right->setEnabled(bar->value()<bar->maximum());
                };
                connect(bar,&QScrollBar::rangeChanged,head,updateArrows);
                connect(bar,&QScrollBar::valueChanged,head,updateArrows);
                connect(bar,&QScrollBar::valueChanged,this,[this] { imageRefresh->start(0); });
                updateArrows();
            }
        }
        Row row; row.strip=strip; row.widget=new QWidget(strip?strip->widget():parent);
        row.widget->setObjectName(strip?"selectionVariant":"selectionRow"); row.widget->setProperty("optionId",item.id);
        if (strip) row.widget->setFixedWidth(274);
        auto* line=new QBoxLayout(strip?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight,row.widget);
        line->setContentsMargins(strip?10:18,strip?8:14,strip?10:14,strip?8:14); line->setSpacing(strip?8:0);
        auto* textArea=new QWidget(row.widget); textArea->setMinimumWidth(60); auto* textBox=new QVBoxLayout(textArea);
        textBox->setContentsMargins(0,0,strip?0:18,0); textBox->setSpacing(7);
        auto* heading=label(item.title,textArea,true); heading->setTextInteractionFlags(Qt::TextSelectableByMouse); textBox->addWidget(heading);
        if (strip) heading->setMinimumHeight(40);
        if (!item.recommendation.isEmpty()) {
            auto* advice=label(item.recommendation,textArea); advice->setObjectName("selectionAdvice"); textBox->addWidget(advice);
        }
        const auto imagePath=item.image.isEmpty() && Selection::isImagePath(item.asset)?item.asset:item.image;
        if (!imagePath.isEmpty()) {
            row.image=new SelectionImage(imagePath,textArea);
            textBox->addWidget(row.image);
        }
        if (!imagePath.isEmpty() || !item.asset.isEmpty()) {
            auto* links=new QHBoxLayout;
            auto addPreview=[this,textArea,links,&item](const QString& path,const QString& caption,const QString& name) {
                auto* button=new QToolButton(textArea);
                button->setText(caption); button->setObjectName(name+"-"+item.id);
                button->setAccessibleName(caption+": "+item.title);
                connect(button,&QToolButton::clicked,this,[this,path,title=item.title] {
                    if (openPreview) openPreview(path,title);
                });
                links->addWidget(button);
            };
            if (!imagePath.isEmpty()) addPreview(imagePath,"Image","selection-open-image");
            if (!item.asset.isEmpty() && item.asset!=imagePath)
                addPreview(item.asset,Selection::isImagePath(item.asset)?"Asset image":"3D model","selection-open-asset");
            links->addStretch(); textBox->addLayout(links);
        }
        if (!strip && !item.markdown.isEmpty()) textBox->addWidget(new SelectionMarkdown(item.markdown,textArea));
        textBox->addStretch(); line->addWidget(textArea,strip?0:1);
        auto* actions=new QWidget(row.widget); actions->setObjectName(strip?"selectionVariantActions":"selectionActions");
        if (!strip) actions->setFixedWidth(142);
        auto* box=new QBoxLayout(strip?QBoxLayout::LeftToRight:QBoxLayout::TopToBottom,actions);
        box->setContentsMargins(strip?0:14,strip?6:0,0,0); box->setSpacing(7);
        auto* pair=new QHBoxLayout; pair->setContentsMargins(0,0,0,0); pair->setSpacing(9);
        row.accept=new DecisionButton(true,actions); row.deny=new DecisionButton(false,actions);
        row.accept->setObjectName("accept-"+item.id); row.deny->setObjectName("deny-"+item.id);
        row.accept->setAccessibleName("Accept: "+item.title); row.deny->setAccessibleName("Deny: "+item.title);
        pair->addWidget(row.accept); pair->addWidget(row.deny); pair->addStretch(); box->addLayout(pair);
        auto* status=new QVBoxLayout;
        status->setSpacing(3);
        row.state=label("Undecided",actions); row.state->setAlignment(Qt::AlignCenter); status->addWidget(row.state);
        row.reset=new QToolButton(actions); row.reset->setText("Clear choice"); row.reset->setAccessibleName("Clear choice: "+item.title); row.reset->setObjectName("clear-"+item.id);
        status->addWidget(row.reset,0,Qt::AlignHCenter);
        box->addLayout(status); box->addStretch();
        line->addWidget(actions);
        if (strip) {
            if (!item.markdown.isEmpty()) line->addWidget(new SelectionMarkdown(item.markdown,row.widget));
            line->addStretch();
        }
        connect(row.accept,&QToolButton::clicked,this,[this,i] { choose(i,"accepted"); }); connect(row.deny,&QToolButton::clicked,this,[this,i] { choose(i,"denied"); });
        connect(row.reset,&QToolButton::clicked,this,[this,i] { choose(i,"pending"); });
        rows.append(row);
        if (variants) variants->addWidget(row.widget);
        else contents->addWidget(row.widget);
    }
    empty=label("No options match",parent); empty->setContentsMargins(18,25,18,25); contents->addWidget(empty); contents->addStretch();
}
void SelectionView::readResults() {
    if (document.path.isEmpty()) return;
    QJsonObject nextResults; QString error;
    const bool validResults=Selection::readDecisions(resultPath,document,nextResults,error);
    writable=sourceValid && validResults;
    if (sourceValid) resultError=error;
    if (validResults) results=std::move(nextResults);
    if (!watcher.files().isEmpty()) watcher.removePaths(watcher.files());
    if (!watcher.directories().isEmpty()) watcher.removePaths(watcher.directories());
    if (QFileInfo(resultPath).isFile()) watcher.addPath(resultPath);
    if (QFileInfo(QFileInfo(resultPath).absolutePath()).isDir()) watcher.addPath(QFileInfo(resultPath).absolutePath());
    refresh();
}
void SelectionView::choose(int index,const QString& value) {
    if (!writable || index<0 || index>=document.items.size()) return;
    const auto& item=document.items[index];
    const auto desired=Selection::decision(results,item)==value?QString("pending"):value;
    QString error; QJsonObject saved;
    if (Selection::writeDecision(resultPath,document,item.id,desired,saved,error)) {
        results=std::move(saved); resultError.clear();
    } else {
        resultError=error;
        if (error=="Options changed; reload before deciding") { sourceValid=false; writable=false; }
    }
    refresh();
}
void SelectionView::navigate(int delta) {
    current+=delta; refresh(); scroll->verticalScrollBar()->setValue(0);
}
void SelectionView::refresh() {
    int accepted=0,denied=0,stale=0;
    QVector<int> visible;
    const QString wanted=filter->currentIndex()==1?"pending":filter->currentIndex()==2?"accepted":filter->currentIndex()==3?"denied":"";
    const auto query=search->text().trimmed();
    for (int i=0;i<document.items.size();++i) {
        const auto& item=document.items[i]; const auto value=Selection::decision(results,item);
        accepted+=value=="accepted"; denied+=value=="denied";
        const auto record=results["decisions"].toObject()[item.id].toObject();
        stale+=!record.isEmpty() && record["digest"]!=item.digest;
        if ((wanted.isEmpty() || value==wanted) && (query.isEmpty() || (item.title+" "+item.markdown+" "+item.sectionTitle+" "+item.recommendation).contains(query,Qt::CaseInsensitive))) visible.append(i);
        auto& row=rows[i]; row.accept->setChecked(value=="accepted"); row.deny->setChecked(value=="denied");
        row.accept->setEnabled(writable); row.deny->setEnabled(writable); row.reset->setEnabled(writable && value!="pending");
        row.state->setText(value=="accepted"?"Accepted":value=="denied"?"Denied":"Undecided");
    }
    current=std::clamp(current,0,std::max(0,int(visible.size())-1));
    const bool single=mode->currentIndex()==1;
    QSet<QString> visibleSections;
    for (int i=0;i<rows.size();++i) {
        const bool show=single?(!visible.isEmpty() && visible[current]==i):visible.contains(i);
        rows[i].widget->setVisible(show); if (show) visibleSections.insert(document.items[i].section);
    }
    for (auto* section:sections) section->setVisible(visibleSections.contains(section->property("sectionId").toString()));
    previous->setVisible(single); next->setVisible(single); position->setVisible(single);
    previous->setEnabled(current>0); next->setEnabled(current+1<visible.size());
    position->setText(QString("%1 / %2").arg(visible.isEmpty()?0:current+1).arg(visible.size()));
    counts->setText(QString("%1 accepted   ·   %2 denied   ·   %3 undecided").arg(accepted).arg(denied).arg(document.items.size()-accepted-denied));
    counts->setToolTip(resultPath);
    notice->setText(!resultError.isEmpty()?resultError:stale?QString("%1 changed option(s) need a new choice.").arg(stale):QString()); notice->setVisible(!notice->text().isEmpty());
    empty->setText(document.items.isEmpty()?"No options in this selection":"No options match this filter"); empty->setVisible(visible.isEmpty());
    imageRefresh->start(0);
}
void SelectionView::refreshImages() {
    auto* viewport=scroll->viewport();
    for (const auto& row:rows) if (row.image) {
        const QRect bounds(row.image->mapTo(viewport,QPoint()),row.image->size());
        auto visible=viewport->rect();
        if (row.strip) {
            auto* horizontal=row.strip->viewport();
            visible=visible.intersected(QRect(horizontal->mapTo(viewport,QPoint()),horizontal->size()));
        }
        row.image->setInViewport(row.image->isVisible() && visible.intersects(bounds));
    }
}
void SelectionView::theme() {
    if (changingTheme) return;
    const QScopedValueRollback guard(changingTheme,true);
    const bool dark=palette().color(QPalette::Window).lightness()<128;
    const QString border=dark?"#353637":"#d9d9d9",muted=dark?"#b3b5b7":"#5c6064";
    setStyleSheet(QString("QWidget#selectionView, QWidget#selectionContent {background:%1;}"
        "QWidget#selectionRow {border-top:1px solid %2;} QWidget#selectionSection {border-top:1px solid %2;}"
        "QWidget#selectionActions {border-left:1px solid %2;} QWidget#selectionVariant {border-right:1px solid %2;}"
        "QWidget#selectionVariantActions {border-top:1px solid %2;} QLabel#selectionCounts {border-top:1px solid %2;border-bottom:1px solid %2;color:%3;}"
        "QLabel#selectionAdvice {color:%3;} QLabel#selectionNotice {color:%4;}"
        "QScrollBar:vertical {background:transparent;width:8px;margin:0;} QScrollBar::handle:vertical {background:transparent;min-height:30px;border:2px solid transparent;border-radius:3px;}"
        "QScrollBar:vertical:hover::handle, QScrollBar:vertical:focus::handle {background:%5;}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {height:0;} QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical {background:transparent;}"
        "QScrollBar:horizontal {background:transparent;height:10px;margin:0;} QScrollBar::handle:horizontal {background:%5;min-width:28px;border:2px solid transparent;border-radius:4px;}"
        "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal {width:0;} QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal {background:transparent;}")
        .arg(dark?"#222222":"#fafafa",border,muted,dark?"#edbe7a":"#845313",dark?"#737373":"#8e8e8e"));
}
void SelectionView::resizeEvent(QResizeEvent* e) { QWidget::resizeEvent(e); imageRefresh->start(0); }
void SelectionView::showEvent(QShowEvent* e) { QWidget::showEvent(e); imageRefresh->start(0); }
void SelectionView::changeEvent(QEvent* e) { QWidget::changeEvent(e); if (e->type()==QEvent::PaletteChange) theme(); }
void SelectionView::fit() { scroll->verticalScrollBar()->setValue(0); }
QJsonObject SelectionView::metrics() const {
    QJsonArray choices; int accepted=0,denied=0;
    for (const auto& item:document.items) {
        const auto value=Selection::decision(results,item); accepted+=value=="accepted"; denied+=value=="denied";
        choices.append(QJsonObject{{"id",item.id},{"decision",value},{"digest",item.digest}});
    }
    int images=0,ready=0,assets=0;
    for (const auto& row:rows) if (row.image) { ++images; ready+=row.image->ready(); }
    for (const auto& item:document.items) assets+=!item.asset.isEmpty();
    return {{"loads",loads},{"selection_id",document.id},{"selection_title",document.title},{"selection_mode",mode->currentIndex()==1?"single":"batch"},
        {"selection_presentation",document.variantRows?"variant-rows":"list"},
        {"selection_images",images},{"selection_images_ready",ready},{"selection_assets",assets},
        {"selection_total",document.items.size()},{"selection_accepted",accepted},{"selection_denied",denied},{"selection_pending",document.items.size()-accepted-denied},
        {"selection_decisions",choices},{"selection_result_path",resultPath},{"selection_writable",writable},{"selection_error",resultError}};
}
