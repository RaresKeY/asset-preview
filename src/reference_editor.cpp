#include "reference_editor.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

QString screenshotDirectory() {
    auto pictures=QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (pictures.isEmpty()) pictures=QDir::homePath()+"/Pictures";
    return QDir(pictures).filePath("Asset Preview");
}
QString saveReferencePng(const QImage& image,const QString& label,QString& error) {
    if (image.isNull()) { error="No frame is available yet."; return {}; }
    const auto directory=screenshotDirectory();
    if (!QDir().mkpath(directory)) { error="Cannot create screenshot folder: "+directory; return {}; }
    auto safe=label.left(80); safe.replace(QRegularExpression("[^a-zA-Z0-9_-]"),"-");
    const auto name=QString("Preview-%1-%2-%3.png").arg(safe,QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"),QUuid::createUuid().toString(QUuid::Id128).left(8));
    QFile file(QDir(directory).filePath(name));
    if (!file.open(QIODevice::WriteOnly|QIODevice::NewOnly)) { error=file.errorString(); return {}; }
    if (!image.save(&file,"PNG")) { file.remove(); error="Cannot save screenshot."; return {}; }
    return file.fileName();
}
ReferenceCanvas::ReferenceCanvas(QImage source,QWidget* parent):QWidget(parent),image(std::move(source)) {
    setObjectName("referenceCanvas"); setMinimumSize(240,180); setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    setCursor(Qt::CrossCursor);
}
QRectF ReferenceCanvas::imageRect() const {
    const auto fitted=QSizeF(image.size()).scaled(size(),Qt::KeepAspectRatio);
    return {QPointF((width()-fitted.width())/2,(height()-fitted.height())/2),fitted};
}
QPointF ReferenceCanvas::imagePoint(const QPointF& point) const {
    const auto box=imageRect(); const auto p=(point-box.topLeft())*(image.width()/box.width());
    return {std::clamp(p.x(),0.,double(image.width())),std::clamp(p.y(),0.,double(image.height()))};
}
void ReferenceCanvas::drawMarks(QPainter& p) const {
    const qreal unit=std::max(1.,std::max(image.width(),image.height())/1100.);
    p.setRenderHint(QPainter::Antialiasing);
    for (const auto& m:marks) {
        p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor("#ff4646"),3*unit,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        if (m.tool==Pen) p.drawPath(m.path);
        if (m.tool==Ellipse) p.drawEllipse(QRectF(m.start,m.end).normalized());
        if (m.tool==Highlight) { p.setPen(Qt::NoPen); p.setBrush(QColor(255,220,0,85)); p.drawRect(QRectF(m.start,m.end).normalized()); }
        if (m.tool==Number) {
            p.setPen(QPen(Qt::white,2*unit)); p.setBrush(QColor("#df3038")); p.drawEllipse(m.start,14*unit,14*unit);
            QFont font=p.font(); font.setPixelSize(qRound(18*unit)); font.setBold(true); p.setFont(font);
            p.drawText(QRectF(m.start-QPointF(15*unit,15*unit),QSizeF(30*unit,30*unit)),Qt::AlignCenter,QString::number(m.number));
        }
        if (m.tool==Text) {
            QFont font=p.font(); font.setPixelSize(qRound(23*unit)); font.setBold(true); p.setFont(font);
            QPainterPath text; text.addText(m.start,font,m.text);
            p.setPen(QPen(Qt::black,3*unit)); p.setBrush(QColor("#fff04a")); p.drawPath(text);
        }
    }
}
QImage ReferenceCanvas::rendered() const {
    auto result=image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter p(&result); drawMarks(p); return result;
}
void ReferenceCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this); p.fillRect(rect(),QColor("#171c22"));
    const auto box=imageRect(); p.setRenderHint(QPainter::SmoothPixmapTransform); p.drawImage(box,image);
    p.setClipRect(box); p.translate(box.topLeft()); p.scale(box.width()/image.width(),box.height()/image.height()); drawMarks(p);
}
void ReferenceCanvas::mousePressEvent(QMouseEvent* e) {
    if (e->button()!=Qt::LeftButton || !imageRect().contains(e->position())) return;
    const auto point=imagePoint(e->position()); Mark mark{tool,{},point,point,{},0};
    if (tool==Text) {
        bool ok=false; mark.text=QInputDialog::getText(this,"Write on reference","Text:",QLineEdit::Normal,{},&ok).left(512);
        if (!ok || mark.text.isEmpty()) return;
    }
    if (tool==Number) { mark.number=1; for (const auto& m:marks) if (m.tool==Number) ++mark.number; }
    if (marks.size()>=512) return;
    mark.path.moveTo(point); marks.append(mark); ++changes;
    drawing=tool==Pen || tool==Ellipse || tool==Highlight; update();
}
void ReferenceCanvas::mouseMoveEvent(QMouseEvent* e) {
    if (!drawing || marks.isEmpty()) return;
    auto& mark=marks.last(); mark.end=imagePoint(e->position());
    if (mark.path.elementCount()<20000) mark.path.lineTo(mark.end);
    ++changes; update();
}
void ReferenceCanvas::mouseReleaseEvent(QMouseEvent* e) { if (drawing) mouseMoveEvent(e); drawing=false; }
void ReferenceCanvas::undo() { drawing=false; if (!marks.isEmpty()) { marks.removeLast(); ++changes; update(); } }

void editReference(QImage image,QString label,QString context,std::function<bool(const QString&,const QString&)> attach,QWidget* parent) {
    if (image.isNull()) return;
    QDialog dialog(parent); dialog.setObjectName("referenceEditor"); dialog.setWindowTitle("Annotate reference · "+label);
    auto* layout=new QVBoxLayout(&dialog); auto* tools=new QHBoxLayout;
    auto* canvas=new ReferenceCanvas(std::move(image));
    const QList<QPair<QString,ReferenceCanvas::Tool>> choices{{"Circle",ReferenceCanvas::Ellipse},{"Pen",ReferenceCanvas::Pen},{"Highlight",ReferenceCanvas::Highlight},{"Number",ReferenceCanvas::Number},{"Text",ReferenceCanvas::Text}};
    for (const auto& choice:choices) {
        auto* button=new QToolButton; button->setText(choice.first); button->setCheckable(true); button->setChecked(choice.second==canvas->tool);
        tools->addWidget(button);
        QObject::connect(button,&QToolButton::clicked,&dialog,[canvas,tools,button,tool=choice.second] {
            canvas->tool=tool;
            for (int i=0;i<tools->count();++i) if (auto* b=qobject_cast<QToolButton*>(tools->itemAt(i)->widget())) b->setChecked(b==button);
        });
    }
    auto* undo=new QPushButton("Undo"); tools->addWidget(undo); tools->addStretch();
    QObject::connect(undo,&QPushButton::clicked,canvas,&ReferenceCanvas::undo);
    layout->addLayout(tools); layout->addWidget(canvas,1);
    auto* status=new QLabel("Add marks if needed. Attach inserts the PNG into the conversation draft."); status->setWordWrap(true); status->setTextFormat(Qt::PlainText); layout->addWidget(status);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close);
    auto* save=buttons->addButton("Save PNG",QDialogButtonBox::ActionRole);
    auto* copy=buttons->addButton("Copy Image",QDialogButtonBox::ActionRole);
    auto* send=buttons->addButton("Attach to Codex",QDialogButtonBox::ActionRole); send->setEnabled(bool(attach));
    layout->addWidget(buttons); QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QString saved; int savedRevision=-1;
    const auto persist=[&] {
        if (saved.isEmpty() || savedRevision!=canvas->revision()) {
            QString error; saved=saveReferencePng(canvas->rendered(),label,error);
            if (saved.isEmpty()) { status->setText(error); return false; }
            savedRevision=canvas->revision();
        }
        status->setText("Saved: "+saved); return true;
    };
    QObject::connect(save,&QPushButton::clicked,&dialog,[&] { persist(); });
    QObject::connect(copy,&QPushButton::clicked,&dialog,[&] { if (persist()) { QApplication::clipboard()->setImage(canvas->rendered()); status->setText("Image copied · "+saved); } });
    QObject::connect(send,&QPushButton::clicked,&dialog,[&] { if (persist()) { if (attach(saved,context)) dialog.accept(); else status->setText("Open the Codex sidebar and wait for its composer before attaching. PNG saved: "+saved); } });
    auto available=parent->screen()->availableGeometry().size(); dialog.resize(QSize(1100,780).boundedTo(available*0.9)); dialog.exec();
}
