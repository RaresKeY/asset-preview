#include "file_view.h"
#include "theme.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextBlock>
#include <QAbstractTextDocumentLayout>
#include <QTextFrame>
#include <cstdio>
#include <stdexcept>
#include <sys/resource.h>

// Opt-in interaction probe over the production FileView. Each sample includes
// the action, posted Qt layout/paint work and a synchronous viewport paint;
// it excludes physical display scanout and service/registry RPC overhead.
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        if(argc>2) throw std::runtime_error("Usage: profile_markdown [document.md|synthetic|images]");
        Theme::apply("dark");
        auto hostLoad=[] { QFile file("/proc/loadavg"); return file.open(QIODevice::ReadOnly)?QString::fromLatin1(file.read(128)).trimmed():QString(); };
        const auto loadBefore=hostLoad();
        QTemporaryDir temp;
        QString path=argc>1?QString::fromLocal8Bit(argv[1]):QString();
        if(path=="images") {
            path=temp.filePath("images.md");
            QFile file(path); if(!file.open(QIODevice::WriteOnly)) throw std::runtime_error("fixture write");
            for(int i=0;i<16;++i) {
                QImage image(1600,1000,QImage::Format_RGB32); image.fill(QColor::fromHsv(i*20,90,100));
                {
                    QPainter painter(&image); painter.setPen(Qt::white);
                    for(int y=0;y<1000;y+=40) painter.drawLine(0,y,1599,y);
                    for(int x=0;x<1600;x+=40) painter.drawLine(x,0,x,999);
                    painter.drawText(QRect(20,20,1560,960),Qt::AlignCenter,QString("Screen %1").arg(i+1));
                }
                const auto name=QString("screen-%1.png").arg(i);
                if(!image.save(temp.filePath(name))) throw std::runtime_error("image fixture write");
                const auto section=QString("## Screen %1\n\n![screen](%2)\n\nAn image-heavy guide exceeds the decoded-pixel cache.\n\n").arg(i+1).arg(name).toUtf8();
                if(file.write(section)!=section.size()) throw std::runtime_error("fixture write failed");
            }
        }
        if(path.isEmpty() || path=="synthetic") {
            path=temp.filePath("large.md");
            QFile file(path); if(!file.open(QIODevice::WriteOnly)) throw std::runtime_error("fixture write");
            for(int i=0;i<160;++i) {
                const auto section=QString("## Section %1\n\nRead **formatted** text and `inline code` with a [guide](guide.md). Long paragraphs wrap naturally across narrower document cards.\n\n> Quoted paragraph.\n\n```cpp\nint value_%1 = %1;\nreturn value_%1;\n```\n\n| View | Use |\n|---|---|\n| Rendered | Read documents |\n| Source | Inspect text |\n\n").arg(i).toUtf8();
                if(file.write(section)!=section.size()) throw std::runtime_error("fixture write failed");
            }
        }
        QFile input(path); if(!input.open(QIODevice::ReadOnly)) throw std::runtime_error("read input");
        const auto bytes=input.readAll();
        FileView view; view.resize(1000,680); view.show();
        // Include finite resize callbacks and the geometry work they post;
        // stopping after one event turn can undercount deferred layout.
        auto settle=[&] {
            for(int i=0;i<4;++i) { QApplication::sendPostedEvents(); app.processEvents(); }
        };
        QElapsedTimer clock; clock.start(); QString error;
        if(!view.reload(QJsonObject{{"path",path}},error)) throw std::runtime_error(error.toStdString());
        if(view.metrics()["file_mode"]!="rendered") throw std::runtime_error("Expected a Markdown document in Rendered mode");
        settle(); auto* browser=view.findChild<QTextBrowser*>("fileMarkdown"); browser->viewport()->repaint();
        auto lastContent=browser->document()->lastBlock();
        while(lastContent.isValid() && lastContent.text().isEmpty()) lastContent=lastContent.previous();
        const double contentHeight=lastContent.isValid()?browser->document()->documentLayout()->blockBoundingRect(lastContent).bottom():0;
        if(contentHeight<=browser->viewport()->height()) throw std::runtime_error("Expected a scrollable Markdown document");
        const double cold=clock.nsecsElapsed()/1e6;
        std::fprintf(stderr,"Cold ready: %.3f ms\n",cold); std::fflush(stderr);
        QJsonObject operations,operationDecodes;
        const auto coldDecodes=view.metrics().value("file_image_decodes").toInt();
        for(const auto& operation:{QString("scroll"),QString("resize")}) {
            const auto decodesBefore=view.metrics().value("file_image_decodes").toInt();
            QJsonArray samples;
            for(int i=0;i<26;++i) {
                clock.restart();
                if(operation=="scroll") browser->verticalScrollBar()->setValue((i%9)*browser->verticalScrollBar()->maximum()/8);
                else view.resize(i%2?1000:640,680);
                settle(); browser->viewport()->repaint();
                const double ms=clock.nsecsElapsed()/1e6;
                if(i>=6) samples.append(ms);
                std::fprintf(stderr,"%s %d: %.3f ms\n",qPrintable(operation),i,ms); std::fflush(stderr);
            }
            operations[operation]=samples;
            operationDecodes[operation]=view.metrics().value("file_image_decodes").toInt()-decodesBefore;
        }
        const auto interactionMetrics=view.metrics();
        QJsonObject pixels;
        const auto evidence=qEnvironmentVariable("ASSET_PREVIEW_MARKDOWN_CAPTURES");
        if(!evidence.isEmpty()) {
            if(!QDir().mkpath(evidence)) throw std::runtime_error("Cannot create capture directory");
            for(const auto& theme:{QString("dark"),QString("light")}) for(int width:{1000,640}) {
                Theme::apply(theme); view.resize(width,680); settle();
                for(int percent:{0,50,100}) {
                    browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum()*percent/100); settle();
                    auto image=browser->viewport()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
                    const auto name=QString("%1-%2-%3").arg(theme).arg(width).arg(percent);
                    const QByteArrayView bytes(reinterpret_cast<const char*>(image.constBits()),image.sizeInBytes());
                    pixels[name]=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
                    const auto target=evidence+"/"+name+".png";
                    if(QFile::exists(target) || image.isNull() || !image.save(target)) throw std::runtime_error("Cannot save a new viewport capture");
                }
            }
        }
        QOpenGLContext context; if(!context.create()) throw std::runtime_error("GL context failed");
        QOffscreenSurface surface; surface.setFormat(context.format()); surface.create();
        if(!context.makeCurrent(&surface)) throw std::runtime_error("GL surface failed");
        const auto renderer=QString::fromLatin1(reinterpret_cast<const char*>(context.functions()->glGetString(GL_RENDERER)));
        if(renderer.isEmpty() || renderer.contains("llvmpipe",Qt::CaseInsensitive) || renderer.contains("softpipe",Qt::CaseInsensitive)) throw std::runtime_error("hardware renderer required");
        struct rusage usage{};
        if(getrusage(RUSAGE_SELF,&usage)!=0) throw std::runtime_error("Cannot read peak memory usage");
        // Native frames add structural empty paragraphs. Compare nonempty
        // blocks verbatim so code indentation and inline whitespace still count.
        QStringList logicalBlocks;
        for(auto block=browser->document()->begin();block.isValid();block=block.next())
            if(!block.text().isEmpty()) logicalBlocks.append(block.text());
        QJsonObject output{{"input_bytes",bytes.size()},{"input_sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())},
            {"text_sha256",QString::fromLatin1(QCryptographicHash::hash(browser->toPlainText().toUtf8(),QCryptographicHash::Sha256).toHex())},
            {"logical_text_sha256",QString::fromLatin1(QCryptographicHash::hash(logicalBlocks.join('\n').toUtf8(),QCryptographicHash::Sha256).toHex())},
            {"cold_ms",cold},{"cold_image_decodes",coldDecodes},{"samples_ms",operations},
            {"operation_image_decodes",operationDecodes},{"renderer",renderer},{"qt",qVersion()},
            {"image_decodes",interactionMetrics.value("file_image_decodes")},{"image_cache_cost",interactionMetrics.value("file_image_cache_cost")},
            {"pixel_sha256",pixels},{"peak_rss_kib",qint64(usage.ru_maxrss)},{"load_before",loadBefore},{"load_after",hostLoad()},
            {"scope","native scroll/resize handler, posted Qt events and synchronous viewport paint; six warmups excluded; no scanout or service persistence"}};
        std::puts(QJsonDocument(output).toJson(QJsonDocument::Compact).constData());
    } catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
