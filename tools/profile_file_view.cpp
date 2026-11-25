#include "file_view.h"
#include "theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTemporaryDir>
#include <QTextEdit>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

// Opt-in native-view benchmark. Timings cover the synchronous button handler
// and, separately, drained Qt layout/paint work. Neither is physical scanout
// latency; service validation, registry persistence and watchers are excluded.
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        Theme::apply("dark");
        const int largest=argc>1?QString::fromLocal8Bit(argv[1]).toInt():1000;
        if(largest<200 || largest>10000) throw std::runtime_error("Expected largest row count from 200 to 10000");
        auto load=[] { QFile file("/proc/loadavg"); return file.open(QIODevice::ReadOnly)?QString::fromLatin1(file.read(128)).trimmed():QString(); };
        const auto loadBefore=load();
        QTemporaryDir temporary;
        auto write=[](const QString& path,const QString& text) {
            QFile file(path);
            if(!file.open(QIODevice::WriteOnly) || file.write(text.toUtf8())<0) throw std::runtime_error("fixture write failed");
        };
        auto drain=[&](QWidget& view) {
            for(int i=0;i<4;++i) { QApplication::sendPostedEvents(); app.processEvents(); }
            view.repaint(); app.processEvents();
        };
        QJsonArray cases;
        for(int count:{200,largest}) {
            QString before,after;
            for(int i=0;i<count;++i) {
                const auto line=QString("int value_%1 = %2; // ").arg(i).arg(i)+(i%13==0?QString("long comment ").repeated(24):QString("comment"))+"\n";
                before+=line;
                after+=i%97==0?QString("int value_%1 = %2; // replacement\n").arg(i).arg(i+1):line;
            }
            const auto a=temporary.filePath("before.cpp"),b=temporary.filePath("after.cpp"); write(a,before); write(b,after);
            for(const auto& scenario:{QString("source"),QString("inline-collapsed"),QString("inline-full"),QString("split-full")}) {
                std::fprintf(stderr,"Profiling %d rows, %s\n",count,qPrintable(scenario));
                FileView view; view.resize(1000,680); view.show();
                const auto mode=scenario.startsWith("inline")?QString("inline"):scenario.startsWith("split")?QString("split"):QString("source");
                QJsonObject config{{"path",b},{"settings",QJsonObject{{"file_mode",mode},{"file_context",scenario.endsWith("full")}}}};
                if(mode!="source") config["before"]=a;
                QString error; QElapsedTimer initial; initial.start();
                if(!view.reload(config,error)) throw std::runtime_error(error.toStdString());
                drain(view); const double cold=initial.nsecsElapsed()/1e6;
                std::fprintf(stderr,"Cold ready %.2f ms\n",cold);
                auto* wrap=view.findChild<QCheckBox*>("fileWrap");
                QJsonArray samples;
                for(int i=0;i<6;++i) {
                    QElapsedTimer clock; clock.start(); wrap->click();
                    const double handler=clock.nsecsElapsed()/1e6;
                    drain(view);
                    const double settled=clock.nsecsElapsed()/1e6;
                    std::fprintf(stderr,"Wrap %d: handler %.2f ms, settled %.2f ms\n",wrap->isChecked(),handler,settled);
                    if(i>=2) samples.append(QJsonObject{{"wrap",wrap->isChecked()},{"handler_ms",handler},{"qt_settled_ms",settled}});
                }
                QByteArray text;
                for(const auto& name:mode=="split"?QStringList{"fileBefore","fileAfter"}:QStringList{mode=="inline"?"fileInline":"fileSource"})
                    text+=view.findChild<QTextEdit*>(name)->toPlainText().toUtf8()+QByteArray(1,'\0');
                cases.append(QJsonObject{{"rows",count},{"scenario",scenario},{"cold_ms",cold},{"samples",samples},
                    {"final_text_sha256",QString::fromLatin1(QCryptographicHash::hash(text,QCryptographicHash::Sha256).toHex())}});
                view.hide(); app.processEvents();
            }
        }
        QOpenGLContext context; if(!context.create()) throw std::runtime_error("GL context failed");
        QOffscreenSurface surface; surface.setFormat(context.format()); surface.create();
        if(!context.makeCurrent(&surface)) throw std::runtime_error("GL surface failed");
        const auto renderer=QString::fromLatin1(reinterpret_cast<const char*>(context.functions()->glGetString(GL_RENDERER)));
        if(renderer.isEmpty() || renderer.contains("llvmpipe",Qt::CaseInsensitive) || renderer.contains("softpipe",Qt::CaseInsensitive)) throw std::runtime_error("hardware renderer required");
        const QJsonObject result{{"renderer",renderer},{"qt",qVersion()},{"viewport","1000x680"},{"load_before",loadBefore},{"load_after",load()},
            {"scope","Native FileView button handler and drained Qt paint; excludes service persistence and physical presentation"},{"cases",cases}};
        std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    } catch(const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
