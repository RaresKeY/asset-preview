#include "material_graph.h"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QGraphicsObject>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {
bool jsonFile(const QString& path, QJsonObject& value, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error=file.errorString(); return false; }
    if (file.size()>4*1024*1024) { error="Graph exceeds 4 MiB"; return false; }
    QJsonParseError parse;
    const auto document=QJsonDocument::fromJson(file.readAll(),&parse);
    if (parse.error!=QJsonParseError::NoError || !document.isObject()) {
        error="Invalid graph JSON: "+parse.errorString(); return false;
    }
    value=document.object(); return true;
}
QString resolved(const QString& base,QString path) {
    if (path.isEmpty()) return {};
    path.replace("%PROJECT_PATH%",QFileInfo(base).absolutePath());
    return QDir::cleanPath(QFileInfo(path).isAbsolute()?path:QFileInfo(base).dir().absoluteFilePath(path));
}
QString pretty(QString s) { s.replace('_',' '); if (!s.isEmpty()) s[0]=s[0].toUpper(); return s; }
QColor nodeColor(const QString& kind) {
    if (kind=="material") return QColor("#b8b8b8");
    if (kind=="image") return QColor("#b7a58c");
    if (kind=="channel") return QColor("#aaa3b9");
    return QColor("#9eafb8");
}
QString stamp(const QString& path) {
    const QFileInfo f(path); return QString::number(f.lastModified().toMSecsSinceEpoch())+":"+QString::number(f.size());
}
class GraphCanvas final : public QGraphicsView {
public:
    using QGraphicsView::QGraphicsView;
    bool fitted=true;
    void fitGraph() { fitInView(scene()->sceneRect(),Qt::KeepAspectRatio); if(transform().m11()>1)resetTransform(); }
protected:
    void resizeEvent(QResizeEvent* event) override { QGraphicsView::resizeEvent(event); if(fitted)fitGraph(); }
    void wheelEvent(QWheelEvent* event) override {
        if (event->modifiers() & Qt::ControlModifier) {
            fitted=false;
            const double next=transform().m11()*std::pow(1.0015,event->angleDelta().y());
            if (next>.15 && next<3) scale(next/transform().m11(),next/transform().m11());
            event->accept();
        } else QGraphicsView::wheelEvent(event);
    }
};
class Card final : public QGraphicsObject {
public:
    MaterialGraph::Node node;
    QImage image;
    bool expanded=false, selected=false;
    std::function<void()> clicked, toggled;
    Card(MaterialGraph::Node n,QImage i,bool e,bool s):node(std::move(n)),image(std::move(i)),expanded(e),selected(s) {
        setAcceptHoverEvents(true); setFlag(ItemIsFocusable); setCursor(Qt::PointingHandCursor);
        QString information=node.label+"\n"+node.description+"\n"+node.kind+" · "+node.state;
        if(!node.preview.isEmpty())information+="\n"+node.preview+"\nSaved: "+QFileInfo(node.preview).lastModified().toString(Qt::ISODate);
        if(!node.parameters.isEmpty())information+="\n"+QString::fromUtf8(QJsonDocument(node.parameters).toJson(QJsonDocument::Compact)).left(400);
        setToolTip(information+"\nClick to inspect · double click to expand");
    }
    QRectF boundingRect() const override { return {0,0,178,expanded?276.:node.kind=="channel"?126.:174.}; }
    void paint(QPainter* p,const QStyleOptionGraphicsItem*,QWidget*) override {
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(selected?QColor("#b8b8b8"):QColor("#444444"),selected?2:1));
        p->setBrush(QColor("#292929")); p->drawRoundedRect(boundingRect(),3,3);
        const QColor accent=nodeColor(node.kind);
        p->setPen(accent); QFont font=p->font(); font.setPixelSize(10); font.setBold(true); p->setFont(font);
        p->drawText(QRectF(12,8,152,16),Qt::AlignLeft,node.kind.toUpper());
        p->setPen(QColor("#e9e9e9")); font.setPixelSize(12); p->setFont(font);
        p->drawText(QRectF(12,27,154,19),Qt::AlignLeft,p->fontMetrics().elidedText(node.label,Qt::ElideRight,154));
        const QRectF preview(12,53,154,node.kind=="channel"&&!expanded?44:87);
        p->setPen(Qt::NoPen); p->setBrush(QColor("#202020")); p->drawRoundedRect(preview,2,2);
        if (!image.isNull()) {
            p->save(); QPainterPath clip; clip.addRoundedRect(preview,2,2); p->setClipPath(clip);
            p->drawImage(preview,image); p->restore();
        } else {
            p->setPen(QColor("#929292")); font.setBold(false); font.setPixelSize(11); p->setFont(font);
            p->drawText(preview,Qt::AlignCenter,node.preview.isEmpty()?"Recipe / no saved preview":"Waiting for preview");
        }
        p->setPen(node.state=="error"?QColor("#e99c90"):node.state=="running"?QColor("#eac17e"):QColor("#a4aaa4"));
        font.setPixelSize(10); font.setBold(false); p->setFont(font);
        const auto footer=node.kind=="channel"&&!expanded?103:148;
        p->drawText(QRectF(12,footer,142,16),Qt::AlignLeft,node.state);
        p->setPen(accent); p->drawText(QRectF(152,footer-1,16,16),Qt::AlignCenter,expanded?"−":"+");
        if (expanded) {
            p->setPen(QColor("#ababab"));
            QString content=node.description;
            for (auto it=node.parameters.begin();it!=node.parameters.end();++it)
                content+="\n"+it.key()+": "+(it.value().isString()?it.value().toString():QString::fromUtf8(QJsonDocument(QJsonArray{it.value()}).toJson(QJsonDocument::Compact)).mid(1).chopped(1));
            p->drawText(QRectF(12,178,154,86),Qt::TextWordWrap,content.left(240));
        }
        p->setBrush(accent); p->setPen(QPen(QColor("#202020"),2));
        p->drawEllipse(QPointF(0,85),5,5); p->drawEllipse(QPointF(178,85),5,5);
    }
protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* e) override { setFocus(); if(e->pos().x()>145 && e->pos().y()>(node.kind=="channel"&&!expanded?98:140)) { if(toggled)toggled(); } else if(clicked)clicked(); e->accept(); }
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* e) override { if(toggled)toggled(); e->accept(); }
    void keyPressEvent(QKeyEvent* e) override { if(e->key()==Qt::Key_Return || e->key()==Qt::Key_Space) { if(clicked)clicked(); e->accept(); } else if(e->key()==Qt::Key_E) { if(toggled)toggled(); e->accept(); } else QGraphicsObject::keyPressEvent(e); }
};
}

namespace MaterialGraph {
QString sidecar(const QString& path) { return path+".live.json"; }
bool read(const QString& path, Document& out, QString& error) {
    QJsonObject source;
    if (!jsonFile(path,source,error)) return false;
    Document d; d.path=path; d.materialId=source["name"].toString(QFileInfo(path).completeBaseName());
    d.title=source["label"].toString(pretty(d.materialId)); d.status="Saved recipe";
    d.dependencies={path,sidecar(path)};
    QJsonObject live;
    if (QFileInfo::exists(sidecar(path)) && !jsonFile(sidecar(path),live,error)) return false;
    d.status=live["status"].toString(d.status); d.operations=live["operations"].toArray();
    if(d.operations.size()>500) { error="Operation log exceeds 500 entries"; return false; }
    const auto raw=source["nodes"].toArray();
    if(raw.isEmpty() || raw.size()>256 || source["connections"].toArray().size()>1024) { error="Expected 1–256 nodes and at most 1024 connections"; return false; }
    // Infer ordinary Atlas output conventions without evaluating procedural shaders.
    QDir project=QFileInfo(path).dir();
    while(!project.exists("project.godot") && project.cdUp()) {}
    const auto relative=QDir(project.filePath("materials")).relativeFilePath(path);
    const auto relativeDir=QFileInfo(relative).path();
    const auto materialDir=project.filePath("assets/materials/")+(relativeDir=="."?QString():relativeDir+"/");
    const QStringList channels{"albedo","normal","orm","height","emission"};
    for(const auto& channel:channels) {
        if(!project.exists("project.godot"))break;
        const QStringList candidates{materialDir+d.materialId+"_"+channel+".png",materialDir+d.materialId+"_"+channel+"map.png",materialDir+d.materialId+"/"+channel+".png",materialDir+d.materialId+"/"+channel+"map.png"};
        for(const auto& candidate:candidates) { d.dependencies.append(candidate); if(QFileInfo::exists(candidate) && !d.maps.contains(channel))d.maps[channel]=candidate; }
    }
    const auto overrides=live["maps"].toObject();
    for(auto it=overrides.begin();it!=overrides.end();++it) { if(!channels.contains(it.key()) || !it.value().isString()) { error="Invalid output map"; return false; } d.maps[it.key()]=resolved(path,it.value().toString()); }
    QMap<QString,QJsonObject> originals;
    QSet<QString> ids;
    const auto patches=live["nodes"].toObject();
    for(const auto& value:raw) {
        const auto n=value.toObject(); const auto id=n["name"].toString(n["id"].toString());
        if(id.isEmpty() || id.size()>256 || ids.contains(id)) { error="Node IDs must be unique, nonempty and at most 256 characters"; return false; }
        ids.insert(id); originals[id]=n;
        Node node; node.id=id; node.label=n["label"].toString(pretty(id)); node.kind=n["type"].toString("operation"); node.parameters=n["parameters"].toObject();
        const auto patch=patches[id].toObject();
        node.description=patch["description"].toString(n["description"].toString());
        node.preview=resolved(path,patch["preview"].toString(node.parameters["image"].toString()));
        if(node.preview.isEmpty() && (node.kind=="material" || node.kind=="shader"))node.preview=d.maps.value("albedo");
        node.state=patch["state"].toString(node.preview.isEmpty()?"Recipe saved":QFileInfo::exists(node.preview)?"Saved preview":"Awaiting output");
        if(patch.contains("parameters")) { const auto extra=patch["parameters"].toObject(); for(auto it=extra.begin();it!=extra.end();++it)node.parameters[it.key()]=it.value(); }
        d.nodes.append(node);
    }
    const QStringList materialPorts{"albedo","metallic","roughness","ao","normal","emission","height"};
    for(const auto& value:source["connections"].toArray()) {
        const auto c=value.toObject(); Edge edge{c["from"].toString(),c["to"].toString(),c["label"].toString(),c["from_port"].toInt(),c["to_port"].toInt()};
        if(!ids.contains(edge.from) || !ids.contains(edge.to) || edge.fromPort<0 || edge.toPort<0) { error="Connection references an unknown node or invalid port"; return false; }
        // Present actual output sockets as preview cards; do not invent shader internals.
        if(originals[edge.to]["type"]=="material" && edge.toPort<materialPorts.size()) {
            const auto channel=materialPorts[edge.toPort]; const auto id="@output:"+edge.to+":"+QString::number(edge.toPort);
            const auto map=d.maps.value(channel=="roughness" || channel=="metallic" || channel=="ao"?"orm":channel);
            if(!ids.contains(id)) {
                ids.insert(id); Node node; node.id=id; node.label=pretty(channel); node.kind="channel"; node.preview=map;
                node.description=(channel=="roughness"?"Green channel of packed ORM":channel=="metallic"?"Blue channel of packed ORM":channel=="ao"?"Red channel of packed ORM":"Saved "+channel+" output")+" · material input "+QString::number(edge.toPort);
                node.state=map.isEmpty() || !QFileInfo::exists(map)?"Awaiting bake":"Saved output"; d.nodes.append(node);
            }
            d.edges.append({edge.from,id,edge.label,edge.fromPort,0}); d.edges.append({id,edge.to,channel,0,edge.toPort});
        } else d.edges.append(edge);
    }
    // Stable topological layout; cycles are rejected, never hidden or guessed.
    QMap<QString,int> degrees; QMap<QString,QStringList> outgoing;
    for(const auto& n:d.nodes) { degrees[n.id]=0; d.levels[n.id]=0; }
    for(const auto& e:d.edges) { ++degrees[e.to]; outgoing[e.from].append(e.to); }
    QStringList ready; for(auto it=degrees.begin();it!=degrees.end();++it)if(it.value()==0)ready.append(it.key());
    int visited=0;
    while(!ready.isEmpty()) { const auto id=ready.takeFirst(); ++visited; for(const auto& to:outgoing[id]) { d.levels[to]=std::max(d.levels[to],d.levels[id]+1); if(--degrees[to]==0)ready.append(to); } }
    if(visited!=d.nodes.size()) { error="Material graph contains a cycle"; return false; }
    if(d.nodes.size()>512) { error="Graph presentation exceeds 512 nodes"; return false; }
    for(const auto& n:d.nodes)if(!n.preview.isEmpty())d.dependencies.append(n.preview);
    for(const auto& map:d.maps)d.dependencies.append(map);
    d.dependencies.removeDuplicates(); out=std::move(d); return true;
}
QStringList dependencies(const QString& path) {
    Document document; QString error; return read(path,document,error)?document.dependencies:QStringList{path,sidecar(path)};
}
}

MaterialGraphView::MaterialGraphView(QWidget* parent):QWidget(parent) {
    QImageReader::setAllocationLimit(64);
    setObjectName("materialGraph"); setAccessibleName("Live material graph");
    setStyleSheet("#materialGraph{background:#202020;color:#e4e4e4;} #materialGraph QLabel{color:#cdcdcd;} #materialGraph QPushButton,#materialGraph QComboBox{background:#292929;color:#dedede;border:1px solid #414141;border-radius:2px;padding:4px 7px;min-height:18px;} #materialGraph QPushButton:hover{background:#333333;border-color:#626262;} #materialGraph QPushButton:focus,#materialGraph QComboBox:focus{border:1px solid #a0a0a0;} #materialGraph QPlainTextEdit{background:#232323;color:#b8b8b8;border:0;font-size:11px;} #materialGraph QSplitter::handle{background:#353535;} #materialGraph QScrollBar{background:#222222;} #materialGraph QScrollBar::handle{background:#505050;min-height:18px;}");
    auto* root=new QVBoxLayout(this); root->setContentsMargins(8,6,8,6); root->setSpacing(5);
    auto* header=new QHBoxLayout; heading=new QLabel("Material graph"); heading->setStyleSheet("font-size:13px;font-weight:600;color:#ededed;"); heading->setMinimumWidth(0);heading->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);header->addWidget(heading,1);
    fitButton=new QPushButton("Fit graph"); fitButton->setToolTip("Fit all nodes · Ctrl+wheel to zoom · scroll to navigate"); header->addWidget(fitButton);
    connect(fitButton,&QPushButton::clicked,this,[this]{fit();}); root->addLayout(header);
    summary=new QLabel("Choose a saved material recipe"); summary->setWordWrap(true); summary->setStyleSheet("font-size:10px;color:#929292;"); root->addWidget(summary);
    contentSplit=new QSplitter(Qt::Horizontal,this);contentSplit->setObjectName("graphSidebarSplit");contentSplit->setHandleWidth(1);contentSplit->setChildrenCollapsible(false);root->addWidget(contentSplit,1);
    scene=new QGraphicsScene(this); graph=new GraphCanvas(scene,contentSplit); graph->setObjectName("graphCanvas");
    graph->setRenderHints(QPainter::Antialiasing|QPainter::SmoothPixmapTransform); graph->setBackgroundBrush(QColor("#1f1f1f"));
    graph->setFrameShape(QFrame::NoFrame); graph->setTransformationAnchor(QGraphicsView::AnchorUnderMouse); graph->setMinimumHeight(140);
    inspectorPanel=new QWidget(contentSplit);inspectorPanel->setObjectName("materialSidebar");inspectorPanel->setMinimumWidth(225);auto* sidebar=new QVBoxLayout(inspectorPanel);sidebar->setContentsMargins(8,0,0,0);sidebar->setSpacing(0);
    inspectorSplit=new QSplitter(Qt::Vertical,inspectorPanel);inspectorSplit->setObjectName("materialInspectorSplit");inspectorSplit->setHandleWidth(1);inspectorSplit->setChildrenCollapsible(false);sidebar->addWidget(inspectorSplit);
    auto* previewPanel=new QWidget(inspectorSplit);previewPanel->setObjectName("materialPreviewPanel");auto* materialColumn=new QVBoxLayout(previewPanel);materialColumn->setContentsMargins(0,0,0,6);materialColumn->setSpacing(5);
    auto* previewTitle=new QLabel("Material preview");previewTitle->setStyleSheet("font-size:11px;font-weight:600;color:#b5b5b5;");materialColumn->addWidget(previewTitle);
    auto* controls=new QHBoxLayout;controls->setSpacing(4);channels=new QComboBox;channels->setObjectName("materialChannel");channels->addItems({"Shaded","Albedo","Normal","ORM","Height"});channels->setAccessibleName("Material preview channel");controls->addWidget(channels,1);
    targets=new QComboBox;targets->setObjectName("materialTarget");targets->setAccessibleName("Apply material to");targets->addItem("Sphere","sphere");targets->addItem("Cube","cube");targets->addItem("Plane","plane");targets->addItem("Open model…","open");targets->setToolTip("Preview saved maps on a shape or apply them to a UV-mapped GLB model");controls->addWidget(targets,1);materialColumn->addLayout(controls);
    materialHost=new QWidget;materialHost->setObjectName("shadedMaterialHost");materialHost->setMinimumSize(130,115);materialLayout=new QVBoxLayout(materialHost);materialLayout->setContentsMargins(0,0,0,0);materialColumn->addWidget(materialHost,1);
    previewStatus=new QLabel("No bake available");previewStatus->setStyleSheet("font-size:10px;color:#969696;");previewStatus->setWordWrap(true);materialColumn->addWidget(previewStatus);
    auto* nodePanel=new QWidget(inspectorSplit);nodePanel->setObjectName("materialNodePanel");auto* information=new QVBoxLayout(nodePanel);information->setContentsMargins(0,7,0,0);information->setSpacing(5);selectionTitle=new QLabel("Node details");selectionTitle->setStyleSheet("font-size:11px;font-weight:600;");selectionTitle->setWordWrap(true);information->addWidget(selectionTitle);
    image=new QLabel("Select a node");image->setAlignment(Qt::AlignCenter);image->setMinimumHeight(65);image->setMaximumHeight(130);image->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Expanding);image->setStyleSheet("background:#191919;border:1px solid #333333;");information->addWidget(image,1);
    details=new QPlainTextEdit;details->setObjectName("nodeParameters");details->setReadOnly(true);details->setMaximumBlockCount(120);details->setMinimumHeight(55);information->addWidget(details,2);
    inspectorSplit->setSizes({310,260});contentSplit->setSizes({950,310});contentSplit->setStretchFactor(0,1);contentSplit->setStretchFactor(1,0);
    connect(channels,&QComboBox::currentIndexChanged,this,[this]{updateMaterial();});
    connect(targets,&QComboBox::activated,this,[this](int index){
        if(targets->itemData(index)=="open"){
            auto project=QFileInfo(doc.path).dir();while(!project.exists("project.godot") && project.cdUp()){}
            const auto path=QFileDialog::getOpenFileName(this,"Apply material to model",modelPath.isEmpty()?project.filePath("assets/models"):modelPath,"UV-mapped models (*.glb)");
            if(!path.isEmpty())previewOnModel(path);else {const QSignalBlocker guard(targets);targets->setCurrentIndex(modelPath.isEmpty()?std::max(0,targets->findData(materialConfig["settings"].toObject()["shape"].toString("sphere"))):targets->findData(modelPath));}
        }else {modelPath=targets->currentData().toString().endsWith(".glb",Qt::CaseInsensitive)?targets->currentData().toString():QString();watchModel();updateMaterial();}
        if(playbackSettingsRequested)playbackSettingsRequested({{"preview_model",modelPath},{"preview_shape",modelPath.isEmpty()?targets->currentData().toString():QString("sphere")}});
    });
    modelDebounce.setSingleShot(true);modelDebounce.setInterval(180);connect(&modelDebounce,&QTimer::timeout,this,[this]{watchModel();updateMaterial();});
    connect(&modelWatcher,&QFileSystemWatcher::fileChanged,this,[this]{modelDebounce.start();});connect(&modelWatcher,&QFileSystemWatcher::directoryChanged,this,[this]{modelDebounce.start();});
    // Pure Qt functional checks have no native OpenGL surface. They still use
    // the same saved-map inspector, without pretending to verify GPU shading.
    if(QGuiApplication::platformName()=="offscreen")channels->setCurrentIndex(1);
}
MaterialGraphView::~MaterialGraphView() { if(material.surface)delete material.surface; }
bool MaterialGraphView::reload(const QJsonObject& config,QString& error) {
    // Leave room for Asset Preview's title/close overlays when embedded.
    layout()->setContentsMargins(8,config.contains("id")?76:6,8,6);
    MaterialGraph::Document candidate;
    if(!MaterialGraph::read(config["path"].toString(),candidate,error)) { summary->setText("Update failed · showing last saved graph · "+error); return false; }
    const bool first=doc.path!=candidate.path || doc.nodes.isEmpty();
    if(first) { expanded.clear(); selected.clear(); }
    doc=std::move(candidate); ++revisions;
    QSet<QString> paths; for(const auto& n:doc.nodes)if(!n.preview.isEmpty())paths.insert(n.preview);
    for(auto it=images.begin();it!=images.end();) { if(!paths.contains(it.key())) { imageStamps.remove(it.key()); it=images.erase(it); } else ++it; }
    for(const auto& path:paths) {
        const auto revision=stamp(path); if(imageStamps.value(path)==revision)continue;
        QImageReader reader(path); reader.setAutoTransform(true); const auto size=reader.size();
        if(size.isValid())reader.setScaledSize(size.scaled(256,256,Qt::KeepAspectRatio));
        const auto loaded=reader.read();
        if(!loaded.isNull()) { images[path]=loaded; imageStamps[path]=revision; }
        else { images.remove(path); imageStamps.remove(path); }
    }
    heading->setText(doc.title);heading->setToolTip(doc.title);summary->setText(QString("%1 · %2 nodes · %3 connections · saved outputs refresh live").arg(doc.status).arg(doc.nodes.size()).arg(doc.edges.size()));
    if(selected.isEmpty() || std::none_of(doc.nodes.begin(),doc.nodes.end(),[this](const auto& n){return n.id==selected;})) {
        selected=doc.nodes.last().id; for(const auto& n:doc.nodes)if(n.kind=="material")selected=n.id;
    }
    drawGraph(first); inspector(); settings(config);updateMaterial();if(documentChanged)documentChanged();return true;
}
void MaterialGraphView::drawGraph(bool autoFit) {
    const auto transform=graph->transform(); const auto center=graph->mapToScene(graph->viewport()->rect().center());
    scene->clear(); QMap<int,QList<MaterialGraph::Node>> columns;
    for(const auto& n:doc.nodes)columns[doc.levels.value(n.id)].append(n);
    QMap<QString,QPointF> positions;
    const auto heightOf=[this](const auto& n){return expanded.contains(n.id)?276:n.kind=="channel"?126:174;};
    double maxHeight=0; for(const auto& column:columns) { double h=0; for(const auto& n:column)h+=heightOf(n)+22; maxHeight=std::max(maxHeight,h); }
    for(auto it=columns.begin();it!=columns.end();++it) {
        double height=0; for(const auto& n:it.value())height+=heightOf(n)+22;
        double y=(maxHeight-height)/2;
        for(const auto& n:it.value()) { positions[n.id]={it.key()*264.,y}; y+=heightOf(n)+22; }
    }
    for(const auto& e:doc.edges) {
        const auto from=positions[e.from]+QPointF(178,85),to=positions[e.to]+QPointF(0,85);
        QPainterPath path(from); const double bend=std::max(35.,(to.x()-from.x())*.5); path.cubicTo(from+QPointF(bend,0),to-QPointF(bend,0),to);
        auto* line=scene->addPath(path,QPen(QColor("#777777"),1.7)); line->setZValue(-1);
        line->setToolTip(QString("%1 [%2] → %3 [%4] %5").arg(e.from).arg(e.fromPort).arg(e.to).arg(e.toPort).arg(e.label));
    }
    for(const auto& n:doc.nodes) {
        auto preview=images.value(n.preview);
        // Packed ORM cards expose the actual channel rather than the combined RGB map.
        if(n.kind=="channel" && !preview.isNull() && (n.label=="Roughness" || n.label=="Metallic" || n.label=="Ao")) {
            preview=preview.convertToFormat(QImage::Format_RGB32);
            for(int y=0;y<preview.height();++y) { auto* pixels=reinterpret_cast<QRgb*>(preview.scanLine(y)); for(int x=0;x<preview.width();++x) { const auto v=n.label=="Roughness"?qGreen(pixels[x]):n.label=="Metallic"?qBlue(pixels[x]):qRed(pixels[x]); pixels[x]=qRgb(v,v,v); } }
        }
        auto* card=new Card(n,preview,expanded.contains(n.id),selected==n.id); scene->addItem(card); card->setPos(positions[n.id]);
        card->clicked=[this,id=n.id]{QTimer::singleShot(0,this,[this,id]{selectNode(id);});};
        card->toggled=[this,id=n.id]{QTimer::singleShot(0,this,[this,id]{expandNode(id);});};
    }
    scene->setSceneRect(scene->itemsBoundingRect().adjusted(-35,-35,35,35));
    if(autoFit)QTimer::singleShot(0,this,[this]{fit();}); else { graph->setTransform(transform); graph->centerOn(center); }
}
void MaterialGraphView::selectNode(const QString& id) { selected=id; for(auto* item:scene->items())if(auto* card=dynamic_cast<Card*>(item)){card->selected=card->node.id==id;card->update();} inspector(); if(activated)activated(); }
void MaterialGraphView::expandNode(const QString& id) { if(expanded.contains(id))expanded.remove(id);else expanded.insert(id); drawGraph(false); }
void MaterialGraphView::inspector() {
    for(const auto& n:doc.nodes)if(n.id==selected) {
        selectionTitle->setText(n.label+"  /  "+n.kind);
        QString text=n.description+"\nState: "+n.state;
        if(!n.preview.isEmpty())text+="\nPreview: "+n.preview+"\nSaved: "+QFileInfo(n.preview).lastModified().toString(Qt::ISODate);
        if(!n.parameters.isEmpty())text+="\n"+QString::fromUtf8(QJsonDocument(n.parameters).toJson(QJsonDocument::Indented));
        details->setPlainText(text.trimmed()); const auto preview=images.value(n.preview);
        if(preview.isNull()) { image->setPixmap({}); image->setText(n.preview.isEmpty()?"Recipe node\nNo saved preview":"Awaiting preview"); }
        else { image->setText({}); image->setPixmap(QPixmap::fromImage(preview).scaled(image->size(),Qt::KeepAspectRatio,Qt::SmoothTransformation)); }
        return;
    }
}
void MaterialGraphView::updateMaterial() {
    if(inspectorPanel->isHidden()) {if(material.surface){delete material.surface;material={};}return;}
    const auto channel=channels->currentIndex()==0?QString("albedo"):channels->currentText().toLower();
    const auto path=doc.maps.value(channel);
    const bool shaded=channels->currentIndex()==0;targets->setEnabled(shaded);
    QJsonObject config{{"kind",shaded?"material":"image"},{"graph_sample",true},{"path",path},{"settings",QJsonObject{{"shape",modelPath.isEmpty()?targets->currentData().toString():QString("sphere")},{"grid",false},{"background","dark"}}}};
    if(shaded && !modelPath.isEmpty())config["sample_model"]=modelPath;
    QJsonObject maps; for(const auto& key:{QString("normal"),QString("orm")})if(doc.maps.contains(key))maps[key]=doc.maps[key]; config["maps"]=maps;
    QString revision=stamp(path);for(const auto& map:maps)revision+=":"+stamp(map.toString());if(shaded && !modelPath.isEmpty())revision+=":"+stamp(modelPath);
    if(material.surface && config==materialConfig && revision==materialStamp)return;
    const bool targetChanged=materialConfig["sample_model"]!=config["sample_model"] || materialConfig["settings"].toObject()["shape"]!=config["settings"].toObject()["shape"];
    if(material.surface && (materialConfig["kind"]!=config["kind"] || targetChanged)) { delete material.surface; material={}; }
    materialConfig=config;
    materialStamp=revision;
    if(path.isEmpty() || !QFileInfo::exists(path)) { if(material.surface) {delete material.surface;material={};} previewStatus->setText("Awaiting saved "+channel+" output"); return; }
    if(shaded && !modelPath.isEmpty() && !QFileInfo::exists(modelPath)){if(material.surface){delete material.surface;material={};}previewStatus->setText("Waiting for model · "+QFileInfo(modelPath).fileName());return;}
    QString error;
    try {
        if(!material.surface) {
            if(config["kind"]=="image") { auto* view=new ImageView(materialHost); material={view,view}; }
            else material=createModel(config,materialHost,[this](bool ok,QString message){previewStatus->setText(ok?(modelPath.isEmpty()?"Live · shaded material":"Live · applied to "+QFileInfo(modelPath).fileName()):"Preview unavailable · "+message);},{});
            materialLayout->addWidget(material.surface);
            material.view->activated=[this]{if(activated)activated();};
        }
        if(!material.view->reload(config,error))previewStatus->setText("Preview unavailable · "+error);
        else if(config["kind"]=="image")previewStatus->setText("Live · saved "+channel+" map");
    } catch(const std::exception& e) { previewStatus->setText("Shaded preview unavailable · "+QString::fromUtf8(e.what())+" · choose a map"); }
}
bool MaterialGraphView::previewOnModel(const QString& path) {
    const auto absolute=QFileInfo(path).absoluteFilePath();
    if(QFileInfo(absolute).isDir() || QFileInfo(absolute).suffix().toLower()!="glb") {previewStatus->setText("Choose a UV-mapped GLB model");return false;}
    modelPath=absolute;int index=targets->findData(absolute);
    if(index<0){targets->addItem(QFileInfo(absolute).fileName(),absolute);index=targets->count()-1;}
    {const QSignalBlocker guard(targets);targets->setCurrentIndex(index);}
    watchModel();updateMaterial();return true;
}
void MaterialGraphView::watchModel() {
    if(!modelWatcher.files().isEmpty())modelWatcher.removePaths(modelWatcher.files());
    if(!modelWatcher.directories().isEmpty())modelWatcher.removePaths(modelWatcher.directories());
    if(modelPath.isEmpty())return;
    if(QFileInfo::exists(modelPath))modelWatcher.addPath(modelPath);
    auto parent=QFileInfo(modelPath).dir();while(!parent.exists() && parent.cdUp()){}modelWatcher.addPath(parent.absolutePath());
}
QJsonObject MaterialGraphView::previewState() const {
    return {{"target",modelPath.isEmpty()?targets->currentData().toString():modelPath},{"channel",channels->currentIndex()},
        {"sidebar",QString::fromLatin1(contentSplit->saveState().toBase64())},{"inspector",QString::fromLatin1(inspectorSplit->saveState().toBase64())}};
}
void MaterialGraphView::restorePreviewState(const QJsonObject& state) {
    const auto target=state["target"].toString("sphere");
    if(target.endsWith(".glb",Qt::CaseInsensitive))previewOnModel(target);else {modelPath.clear();const QSignalBlocker guard(targets);targets->setCurrentIndex(std::max(0,targets->findData(target)));watchModel();}
    if(state.contains("sidebar"))contentSplit->restoreState(QByteArray::fromBase64(state["sidebar"].toString().toLatin1()));
    if(state.contains("inspector"))inspectorSplit->restoreState(QByteArray::fromBase64(state["inspector"].toString().toLatin1()));
    channels->setCurrentIndex(std::clamp(state["channel"].toInt(),0,channels->count()-1));updateMaterial();
}
void MaterialGraphView::settings(const QJsonObject& config) {
    const auto values=config["settings"].toObject();QJsonObject requested;
    for(const auto* key:{"preview_model","preview_shape"})if(values.contains(key))requested[key]=values[key];
    if(requested==requestedPreviewSettings)return;
    requestedPreviewSettings=requested;
    if(!requested["preview_model"].toString().isEmpty())previewOnModel(requested["preview_model"].toString());
    else if(!requested.isEmpty()){modelPath.clear();const QSignalBlocker guard(targets);targets->setCurrentIndex(std::max(0,targets->findData(requested["preview_shape"].toString("sphere"))));watchModel();updateMaterial();}
}
void MaterialGraphView::fit() { auto* canvas=static_cast<GraphCanvas*>(graph);canvas->fitted=true;canvas->fitGraph(); }
void MaterialGraphView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const bool roomy=width()>=650 && height()>=330;
    if(inspectorPanel && inspectorPanel->isHidden()==roomy) {inspectorPanel->setVisible(roomy);QTimer::singleShot(0,this,[this]{updateMaterial();});}
    if(fitButton)fitButton->setVisible(width()>=320);
    if(summary)summary->setVisible(height()>=280);
}
QImage MaterialGraphView::snapshot() {
    auto frame=grab().toImage();
    if(material.view && material.surface) {const auto rendered=material.view->snapshot();if(!rendered.isNull()){QPainter painter(&frame);painter.drawImage(QRect(material.surface->mapTo(this,QPoint()),material.surface->size()),rendered);}}
    return frame;
}
QJsonObject MaterialGraphView::metrics() const {return {{"loads",revisions},{"graph_nodes",doc.nodes.size()},{"graph_connections",doc.edges.size()},{"graph_revisions",revisions},{"selected_node",selected},{"expanded_nodes",expanded.size()},{"decoded_previews",images.size()},{"preview_target",modelPath.isEmpty()?targets->currentData().toString():modelPath},{"inspector_side","right"},{"inspector_visible",!inspectorPanel->isHidden()},{"material_preview",material.view?material.view->metrics():QJsonObject{}}};}
