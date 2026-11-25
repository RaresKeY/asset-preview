#include "selection_document.h"
#include "selection_view.h"
#include "theme.h"
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <cstdio>
#include <stdexcept>
#include <unistd.h>

namespace {
void check(bool value,const QString& why) { if(!value)throw std::runtime_error(why.toStdString()); }
void write(const QString& path,const QByteArray& data) { QFile f(path); check(f.open(QIODevice::WriteOnly),"open fixture"); check(f.write(data)==data.size(),"write fixture"); }
QByteArray bytes(const QJsonObject& obj) { return QJsonDocument(obj).toJson(); }
QJsonObject fixture() {
    return {{"schema","asset-preview-selection/v1"},{"id","release-choices"},{"title","Demo game · Release packages"},
        {"markdown","**Choose what to prepare.** Each check or X saves a choice. Nothing is published.\n\nThe recommendations below are proposals; every option starts undecided."},
        {"sections",QJsonArray{QJsonObject{{"id","formats"},{"title","Package format"},{"markdown","One clear package per download. Preserve the original release archives."},
            {"items",QJsonArray{QJsonObject{{"id","windows"},{"title","Windows loader · portable ZIP"},{"recommendation","Recommended · portable package"},
                {"markdown","Keep the existing Python / Qt desktop.\n\n- Use **local mod imports**.\n- Expand bundled archives for scanning.\n- Retain licenses and corresponding source.\n\n**Before release:** test startup, imports and uninstall with the exact final bytes."}},
                QJsonObject{{"id","linux"},{"title","Linux loader · portable ZIP"},{"recommendation","Optional · separate platform download"},
                    {"markdown","A compact, readable explanation with **Markdown**, `inline code` and a [source link](https://example.invalid).\n\n| Included | Check |\n| --- | --- |\n| Python and Qt | Preserve notices |\n| Steam Proton bridge | Qualify startup |"}}}}}}}};
}
void capture(SelectionView& view,const QString& name) {
    const auto dir=qEnvironmentVariable("ASSET_PREVIEW_EVIDENCE_DIR");
    if (!dir.isEmpty()) { QDir().mkpath(dir); QTest::qWait(80); check(view.grab().save(dir+"/"+name+".png"),"save capture"); }
}
}
int main(int argc,char** argv) {
    QApplication app(argc,argv); Theme::apply("dark");
    try {
        if(qEnvironmentVariableIsSet("ASSET_PREVIEW_GPU_TEST")) {
            QOffscreenSurface surface; surface.create(); QOpenGLContext context;
            check(context.create() && context.makeCurrent(&surface),"hardware OpenGL context");
            const auto renderer=QString::fromLatin1(reinterpret_cast<const char*>(context.functions()->glGetString(GL_RENDERER)));
            check(!renderer.isEmpty() && !renderer.contains("llvmpipe",Qt::CaseInsensitive) && !renderer.contains("softpipe",Qt::CaseInsensitive),"hardware renderer: "+renderer);
            printf("Renderer: %s\n",qPrintable(renderer)); context.doneCurrent();
        }
        QTemporaryDir temp; check(temp.isValid(),"temporary directory");
        const auto path=temp.path()+"/release.selection.json",out=temp.path()+"/release.decisions.json";
        auto source=fixture(); write(path,bytes(source));
        check(Selection::defaultDecisionsPath(path)==out,"default result path");
        Selection::Document doc; QString error; check(Selection::load(path,doc,error),error);
        QJsonObject result; check(Selection::readDecisions(out,doc,result,error),error);
        check(!QFileInfo::exists(out),"reading never creates decisions");
        check(Selection::decision(result,doc.items[0])=="pending","no preaccepted recommendations");
        check(!Selection::writeDecision(path,doc,"windows","accepted",result,error),"cannot overwrite source");
        check(!Selection::writeDecision(out,doc,"unknown","accepted",result,error),"unknown option rejected");
        {
            QLockFile lock(out+".lock"); check(lock.tryLock(),"test lock");
            check(!Selection::writeDecision(out,doc,"windows","accepted",result,error),"concurrent writer lock");
        }
        SelectionView view; view.resize(1180,840); view.show();
        check(view.reload({{"path",path},{"kind","selection"}},error),error); QTest::qWait(80);
        auto* yes=view.findChild<QToolButton*>("accept-windows"); auto* no=view.findChild<QToolButton*>("deny-linux");
        check(yes && no && yes->isVisible(),"large user controls exist");
        check(yes->size()==QSize(56,56),"square controls");
        capture(view,"batch-dark-desktop");
        QTest::mouseClick(yes,Qt::LeftButton); QTest::mouseClick(no,Qt::LeftButton);
        check(view.metrics()["selection_accepted"]==1 && view.metrics()["selection_denied"]==1,"native clicks save opposite decisions");
        check(Selection::readDecisions(out,doc,result,error),error); check(Selection::decision(result,doc.items[0])=="accepted","durable accepted");
        check(Selection::decision(result,doc.items[1])=="denied","merge preserves other choice");
        yes->setFocus(Qt::TabFocusReason); QTest::keyClick(yes,Qt::Key_Space);
        check(view.metrics()["selection_pending"]==1,"keyboard can clear a choice");
        QTest::keyClick(yes,Qt::Key_Space); capture(view,"batch-dark-decided-focus");
        auto* mode=view.findChild<QComboBox*>("selectionMode"); mode->setCurrentIndex(1); QTest::qWait(60);
        check(yes->isVisible() && !no->isVisible(),"single presentation hides other options"); capture(view,"single-dark-desktop");
        view.resize(600,800); QTest::qWait(80); capture(view,"single-dark-narrow");
        auto* scroll=view.findChild<QScrollArea*>(); check(scroll->widget()->width()<=scroll->viewport()->width(),"narrow layout has no horizontal overflow");
        QImage reference(640,400,QImage::Format_RGB32); reference.fill(QColor("#439475"));
        check(reference.save(temp.path()+"/reference.png"),"create preview image");
        write(temp.path()+"/model.glb","model path fixture");
        {
            auto media=source; auto sections=media["sections"].toArray(); auto section=sections[0].toObject();
            auto items=section["items"].toArray(); auto item=items[0].toObject();
            item["image"]="reference.png"; item["asset"]="model.glb"; items[0]=item;
            auto second=items[1].toObject(); second["asset"]="reference.png"; items[1]=second;
            section["items"]=items; sections[0]=section; media["sections"]=sections;
            write(path,bytes(media));
            check(view.reload({{"path",path}},error),error); QTest::qWait(80);
            check(view.metrics()["selection_images"]==2 && view.metrics()["selection_images_ready"]==1,
                  "selection shows the visible inline image, including raster assets");
            check(view.metrics()["selection_accepted"]==1 && view.metrics()["selection_denied"]==1,
                  "presentation changes preserve prior choices");
            QString opened;
            view.openPreview=[&](QString target,QString) { opened=target; };
            QTest::mouseClick(view.findChild<QToolButton*>("selection-open-image-windows"),Qt::LeftButton);
            check(opened==temp.path()+"/reference.png","open image routes local path");
            QTest::mouseClick(view.findChild<QToolButton*>("selection-open-asset-windows"),Qt::LeftButton);
            check(opened==temp.path()+"/model.glb","open model routes local path");
            view.resize(360,800); QTest::qWait(50);
            check(scroll->widget()->width()<=scroll->viewport()->width(),"inline media fits narrow layout");
            capture(view,"selection-media-narrow");
            view.resize(1180,840); capture(view,"selection-media-desktop");
            media["presentation"]="variant-rows"; write(path,bytes(media));
            mode->setCurrentIndex(0);
            check(view.reload({{"path",path}},error),error); QTest::qWait(80);
            auto* variants=view.findChild<QScrollArea*>("selectionVariantStrip");
            auto cards=view.findChildren<QWidget*>("selectionVariant");
            check(variants && cards.size()==2,"one variant strip for the section");
            check(cards[0]->y()==cards[1]->y() && cards[0]->x()<cards[1]->x(),
                  "variants are adjacent on one row");
            check(view.metrics()["selection_accepted"]==1 && view.metrics()["selection_denied"]==1,
                  "changing presentation preserves both choices");
            capture(view,"variant-rows-desktop");
            view.resize(360,800); QTest::qWait(80);
            check(variants->horizontalScrollBar()->maximum()>0,"narrow variant strip scrolls");
            check(scroll->widget()->width()<=scroll->viewport()->width(),"variant strip does not widen checklist");
            QToolButton* pageRight=nullptr;
            for (auto* button:view.findChildren<QToolButton*>())
                if (button->accessibleName()=="Scroll Package format variants right") pageRight=button;
            check(pageRight && pageRight->isVisible() && pageRight->isEnabled(),"overflow has a visible row navigation button");
            QTest::mouseClick(pageRight,Qt::LeftButton);
            check(variants->horizontalScrollBar()->value()>0,"section navigation scrolls its own strip");
            QTest::qWait(50);
            check(cards[0]->y()==cards[1]->y(),"horizontal scrolling preserves the row");
            capture(view,"variant-rows-narrow");
            mode->setCurrentIndex(1);
            view.resize(600,800); view.openPreview={};
            item["image"]="https://example.invalid/reference.png"; items[0]=item;
            section["items"]=items; sections[0]=section; media["sections"]=sections; write(path,bytes(media));
            Selection::Document invalid;
            check(!Selection::load(path,invalid,error),"network preview path rejected");
            write(path,bytes(source)); check(view.reload({{"path",path}},error),error);
            yes=view.findChild<QToolButton*>("accept-windows"); no=view.findChild<QToolButton*>("deny-linux");
        }
        mode->setCurrentIndex(0); Theme::apply("light"); QTest::qWait(60); capture(view,"batch-light-narrow");
        Theme::apply("dark");
        auto* filter=view.findChild<QComboBox*>("selectionFilter"); filter->setCurrentIndex(1);
        check(!yes->isVisible() && !no->isVisible(),"undecided filter"); capture(view,"empty-filter"); filter->setCurrentIndex(0);
        const auto before=QFileInfo(out).size();
        // A source save must invalidate only the changed option, even when the
        // watcher has not yet delivered the new content to the view.
        auto sections=source["sections"].toArray(); auto section=sections[0].toObject(); auto items=section["items"].toArray();
        auto first=items[0].toObject(); first["markdown"]="Changed package bytes require a new review."; items[0]=first; section["items"]=items; sections[0]=section; source["sections"]=sections;
        write(path,bytes(source)); QTest::mouseClick(yes,Qt::LeftButton);
        check(view.metrics()["selection_error"].toString().contains("changed"),"stale-view click refused"); check(QFileInfo(out).size()==before,"stale click preserves file");
        check(view.reload({{"path",path}},error),error);
        check(view.metrics()["selection_pending"]==1 && view.metrics()["selection_denied"]==1,"changed content resets only affected option");
        capture(view,"changed-option");
        SelectionView restored; check(restored.reload({{"path",path}},error),error); check(restored.metrics()["selection_denied"]==1,"new view restores result");
        const QByteArray corrupt="{bad result"; write(out,corrupt); QTest::qWait(100);
        check(!view.metrics()["selection_writable"].toBool(),"corrupt results disable decisions");
        check(!Selection::writeDecision(out,doc,"windows","accepted",result,error),"corrupt results cannot be overwritten");
        QFile verify(out); check(verify.open(QIODevice::ReadOnly) && verify.readAll()==corrupt,"corrupt results retained"); verify.close(); capture(view,"result-error");
        QFile::remove(out); check(::symlink(QFile::encodeName(path).constData(),QFile::encodeName(out).constData())==0,"create output alias");
        check(!Selection::readDecisions(out,doc,result,error),"result symlink rejected"); QFile::remove(out);
        check(::link(QFile::encodeName(path).constData(),QFile::encodeName(out).constData())==0,"create hardlink");
        check(!Selection::readDecisions(out,doc,result,error),"result hardlink rejected"); QFile::remove(out);
        auto duplicate=fixture(); auto ss=duplicate["sections"].toArray(); auto s=ss[0].toObject(); auto ii=s["items"].toArray(); ii.append(ii[0]); s["items"]=ii; ss[0]=s; duplicate["sections"]=ss; write(path,bytes(duplicate));
        check(!Selection::load(path,doc,error),"duplicate stable ids rejected");
        auto empty=fixture(); empty.remove("sections"); empty["items"]=QJsonArray{}; write(path,bytes(empty)); check(view.reload({{"path",path}},error),error); capture(view,"empty-selection");
        auto single=empty; single["items"]=QJsonArray{QJsonObject{{"id","only"},{"title","One decision"},{"markdown","A single reusable prompt with **Markdown**."}}}; write(path,bytes(single));
        check(view.reload({{"path",path}},error),error); check(view.metrics()["selection_total"]==1,"one-option manifest"); capture(view,"one-option");
        printf("Selection document, decisions and native interaction checks passed.\n");
    } catch(const std::exception& error) { fprintf(stderr,"Selection check failed: %s\n",error.what()); return 1; }
    return 0;
}
