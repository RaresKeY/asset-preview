#include "file_document.h"
#include "file_view.h"
#include "markdown_compat.h"
#include "theme.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTextEdit>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextLayout>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <QTextBrowser>
#include <QTextFrame>
#include <QTextImageFormat>
#include <QTextTable>
#include <QTreeWidget>
#include <QToolButton>
#include <QTimer>
#include <random>
#include <stdexcept>
#include <cstdio>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace {
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void write(const QString& path,const QByteArray& value) { QFile f(path); check(f.open(QIODevice::WriteOnly),"write fixture"); f.write(value); }
QString materialize(const FilePreview::Change& change,bool before) {
    QStringList lines;
    int number=1;
    for(const auto& row:change.rows) {
        if(row.kind=='@' || row.kind=='\\') continue;
        if((before && row.kind!='+') || (!before && row.kind!='-')) {
            check((before?row.before:row.after)==number++,"line numbers reconstruct monotonically"); lines.append(row.text);
        }
    }
    return lines.isEmpty()?QString():lines.join('\n')+'\n';
}
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    if(argc==3 && QString::fromLocal8Bit(argv[1])=="--read-clipboard") {
        const auto image=QApplication::clipboard()->image();
        return !image.isNull() && image.save(QString::fromLocal8Bit(argv[2]),"PNG")?0:1;
    }
    try {
        QTemporaryDir temporary;
        const auto path=temporary.filePath("settings.md"),before=temporary.filePath("settings.before.md");
        std::mt19937 random(49201);
        for(int test=0;test<500;++test) {
            QString a,b;
            for(int i=0,n=random()%45;i<n;++i) a+=QString::number(random()%10)+'\n';
            for(int i=0,n=random()%45;i<n;++i) b+=QString::number(random()%10)+'\n';
            const auto change=FilePreview::compare(a,b);
            check(materialize(change,true)==a,"before reconstruction"); check(materialize(change,false)==b,"after reconstruction");
        }
        const auto one=FilePreview::compare("a\nb\nc\n","a\nB\nc\n");
        check(one.added==1 && one.removed==1,"minimal replacement");
        const auto empty=FilePreview::compare("",""); check(empty.rows.isEmpty(),"empty files");
        check(FilePreview::compare("a","a\n").rows.last().kind=='@',"final newline difference");
        QString rewriteA,rewriteB;
        for(int i=0;i<1500;++i) { rewriteA+=QString("old %1\n").arg(i); rewriteB+=QString("new %1\n").arg(i); }
        const auto rewrite=FilePreview::compare(rewriteA,rewriteB);
        check(materialize(rewrite,true)==rewriteA && materialize(rewrite,false)==rewriteB,"bounded large rewrite");
        const QString patch="diff --git a/ui/theme.css b/ui/theme.css\n--- a/ui/theme.css\n+++ b/ui/theme.css\n@@ -724,3 +724,3 @@\n .pane-title {\n-    font-size: 10px;\n+    font-size: 11px;\n }\ndiff --git a/specs/readme.md b/specs/readme.md\n--- a/specs/readme.md\n+++ b/specs/readme.md\n@@ -1 +1,2 @@\n-Old title\n+# File previews\n+Markdown, code and live diffs.\n";
        const auto parsed=FilePreview::parsePatch(patch);
        check(parsed.size()==2 && parsed[0].rows[1].before==724 && parsed[0].added==1 && parsed[1].added==2,"multi-file patch with hunk offsets");
        const auto ambiguous=FilePreview::parsePatch("--- a/test\n+++ b/test\n@@ -1 +1 @@\n--- source\n+++ output\n");
        check(ambiguous.size()==1 && ambiguous[0].removed==1 && ambiguous[0].added==1,"header-like content inside hunk");
        const auto metadata=FilePreview::parsePatch("diff --git a/old.md b/new.md\nsimilarity index 100%\nrename from old.md\nrename to new.md\ndiff --git \"a/caf\\303\\251.png\" \"b/caf\\303\\251.png\"\nBinary files differ\n");
        check(metadata.size()==2 && metadata[0].rows.size()==3 && metadata[1].after==QString::fromUtf8("café.png"),"Git rename/binary metadata and quoted paths");
        const auto huge=FilePreview::parsePatch("--- a/test\n+++ b/test\n@@ -9999999999999999999999 +1 @@\n-bad\n+good\n");
        check(huge.size()==1 && huge[0].added==0,"invalid hunk offsets stay bounded");
        const QString combined="diff --cc merged.cpp\nindex 123,456..789\n--- a/merged.cpp\n+++ b/merged.cpp\n@@@ -1,1 -1,1 +1,1 @@@\n--old\n++merged\n";
        check(FilePreview::parsePatch(combined).isEmpty(),"combined Git patch stays available as source");
        check(FilePreview::parsePatch(QString(combined).replace("diff --cc ","diff --combined ")).isEmpty(),"alternate Git combined header falls back to source");
        check(FilePreview::parsePatch(patch+combined).isEmpty(),"mixed ordinary and combined patch does not silently omit merge edits");
        check(FilePreview::parsePatch("--- a/merged.cpp\n+++ b/merged.cpp\n@@@ -1,1 -1,1 +1,1 @@@\n--old\n++merged\n").isEmpty(),"standalone combined hunk falls back to source");
        check(FilePreview::parsePatch("--- a/merged.cpp\n+++ b/merged.cpp\n@@@@ -1,1 -1,1 -1,1 +1,1 @@@@\n---old\n+++merged\n").isEmpty(),"more than two merge parents also fall back to source");
        write(path,QByteArray("# File previews\n\nRead **Markdown**, inspect code and compare changes.\n\n| View | Use |\n|---|---|\n| Inline | Review edits |\n| Side by side | Compare versions |\n\n```python\ndef preview(path):\n    return {\"kind\": \"file\", \"watch\": True}\n```\n\n- [x] Live file watching\n- [x] Line numbers and syntax colors\n- [x] Inline and side by side diffs\n\n![blocked](https://example.invalid/do-not-fetch.png)\n<script>never execute</script>\n"));
        FilePreview::Document document; QString error;
        check(FilePreview::load(path,{},document,error) && document.markdown,"Markdown detection");
        {
            const auto renderedThumb=FilePreview::thumbnail({{"path",path}},error);
            const auto sourceThumb=FilePreview::thumbnail({{"path",path},{"settings",QJsonObject{{"file_mode","source"}}}},error);
            check(renderedThumb.size()==QSize(160,100) && sourceThumb.size()==QSize(160,100),"bounded document thumbnails");
            check(renderedThumb!=sourceThumb,"Markdown thumbnails render content rather than raw source");
            const auto codePath=temporary.filePath("thumbnail.py");
            const auto prefix=QByteArray("def preview(value):\n    return 42\n").repeated(48);
            write(codePath,prefix+"old tail\n");
            const auto codeThumb=FilePreview::thumbnail({{"path",codePath}},error);
            write(codePath,prefix+"new tail\n");
            check(!codeThumb.isNull() && codeThumb==FilePreview::thumbnail({{"path",codePath}},error),"thumbnail parsing stops at the first 96 lines");
            write(codePath,QByteArray("bad\0binary",10));
            check(FilePreview::thumbnail({{"path",codePath}},error).isNull(),"thumbnail rejects binary content");
        }
        write(before,"# Previous title\n");
        check(FilePreview::load(path,before,document,error) && document.patch,"pair comparison");
        write(before,QByteArray("bad\0binary",10)); check(!FilePreview::load(path,before,document,error),"binary rejection");
        write(before,QByteArray("\xff",1)); check(!FilePreview::load(path,before,document,error),"invalid UTF-8 rejection");
        for(const auto& incomplete:{QByteArray::fromHex("c3"),QByteArray::fromHex("e282"),QByteArray::fromHex("f09f98")}) {
            write(before,"unfinished "+incomplete);
            check(!FilePreview::load(path,before,document,error),"incomplete final UTF-8 sequence rejection");
        }
        write(before,QString::fromUtf8("café € 😀\n").toUtf8());
        check(FilePreview::load(path,before,document,error),"complete multibyte UTF-8 remains valid");
        write(before,QByteArray(FilePreview::MaxBytes+1,'x')); check(!FilePreview::load(path,before,document,error),"size boundary");

        Theme::apply("dark");
        FileView view;
        auto* actions=new QWidget; actions->setFixedSize(116,28);
        auto* status=new QLabel;
        view.setCardWidgets(actions,status); status->hide();
        view.resize(1280,820); view.show();
        QJsonObject config{{"kind","file"},{"path",path}};
        check(view.reload(config,error),"Markdown view reload"); QTest::qWait(80);
        auto* mode=view.findChild<QComboBox*>("fileMode");
        check(view.metrics()["file_mode"]=="rendered","default rendered Markdown");
        QMenu documentMenu;
        view.addNavigationActions(&documentMenu);
        check(documentMenu.actions().isEmpty(),"document menu omits change navigation");
        auto* browser=view.findChild<QTextBrowser*>("fileMarkdown");
        {
            QTextDocument literal;
            const QString opaque="<!-- <br> <img src='comment.png'> -->\n"
                "<!-- multi-line\n<img src='comment2.png'><br>\n-->\n"
                "<script>\n<img src='script.png'><br>\n</SCRIPT >\n"
                "<pre><img src='pre.png'><br></pre>\n"
                "<style><img src='style.png'><br></style>\n"
                "<textarea><img src='textarea.png'><br></textarea>\n"
                "<?instruction <img src='instruction.png'> ?>\n"
                "<![CDATA[<br><img src='cdata.png'>]]>\n"
                "<span title=\"<img src='attribute.png'>\">literal</span>\n"
                "<span title=\"multi-line\n<img src='attribute2.png'><br>\n\">literal</span>\n"
                "<script type='multi-line\n<img src=\"attribute3.png\"><br>\n'>\n<img src='script2.png'><br>\n</script>\n"
                "<img-widget src='custom.png'><br-widget>\n";
            FilePreview::setMarkdown(&literal,"<img src='allowed.png'>\n"+opaque+"<br>tail\n\n[**<img src='linked.png'>**](target)\n");
            const auto plain=literal.toPlainText();
            for(const auto& line:opaque.split('\n',Qt::SkipEmptyParts))
                check(plain.contains(line),"HTML block lexer preserves comments, raw sections, custom tags and quoted examples");
            QVector<QTextImageFormat> images;
            for(auto block=literal.begin();block.isValid();block=block.next())
                for(auto it=block.begin();!it.atEnd();++it)if(it.fragment().charFormat().isImageFormat())images.append(it.fragment().charFormat().toImageFormat());
            check(images.size()==2 && images[0].name()=="allowed.png" && images[1].name()=="linked.png","opaque HTML never creates additional native image objects");
            check(images[1].anchorHref()=="target" && images[1].fontWeight()==QFont::Bold,"HTML image preserves its enclosing Markdown link and emphasis");
            check(plain.contains("\ntail"),"supported tags resume after an opaque HTML section");
            for(const auto& incomplete:{QString("<!-- unfinished"),QString("<script>unfinished"),QString("<span title='unfinished")}) {
                FilePreview::setMarkdown(&literal,"<img src='allowed.png'>\n"+incomplete+"\n\nAfter<br>block\n");
                check(literal.toPlainText().contains("After\nblock"),"unfinished opaque HTML state does not leak into the next Markdown block");
            }

            QTextDocument many; many.setLayoutEnabled(false);
            const QString source=(QString("a<br>").repeated(2000)+'\n').repeated(10);
            QElapsedTimer clock; clock.start(); FilePreview::setMarkdown(&many,source);
            const auto elapsed=clock.elapsed();
            check(many.toRawText().count(QChar::LineSeparator)==20000 && !many.toPlainText().contains(QChar(0xe000)),"large single-paragraph HTML input replaces every marker");
            check(elapsed<8000,"many HTML breaks avoid quadratic block searches");
            std::fprintf(stdout,"Markdown 20,000-break conversion: %lld ms\n",static_cast<long long>(elapsed));
        }
        {
            const auto htmlPath=temporary.filePath("html-compat.md");
            const auto htmlImage=temporary.filePath(QString::fromUtf8("wide & prüfung.png"));
            QImage pixels(200,100,QImage::Format_ARGB32); pixels.fill(QColor("#e68b42"));
            check(pixels.save(htmlImage),"HTML image fixture");
            write(htmlPath,QString::fromUtf8(
                "# HTML compatibility\n\n"
                "**Before**<br>After<BR />third<br class='label'>fourth &amp; final.\n\n"
                "| Portrait | Description |\n| --- | --- |\n"
                "| <img src=\"wide &amp; prüfung.png\" width=80 height=40 alt=\"A &amp; B\"> | **Repair requests**<br>Eligible for repair visits.<br>Pool: `femaleLower`. |\n\n"
                "<img src='wide &amp; prüfung.png' width='90'>\n\n"
                "<img src='wide &amp; prüfung.png' height='30'>\n\n"
                "Literal `<br>` and escaped \\<br> and entity &lt;br&gt;.\n\n"
                "```html\n<br><img src=\"never-load.png\">\n```\n\n"
                "    <br>\n\n"
                "<!-- <br> and <img src='never-load.png'> -->\n\n"
                "<script>literal <br> and <img src='never-load.png'></script>\n").toUtf8());
            FileView html; html.resize(1000,720); html.show();
            check(html.reload(QJsonObject{{"path",htmlPath}},error),"HTML compatibility reload"); QTest::qWait(40);
            auto* page=html.findChild<QTextBrowser*>("fileMarkdown");
            const auto plain=page->toPlainText();
            check(plain.contains("Before\nAfter\nthird\nfourth & final."),"HTML breaks retain all surrounding Markdown text");
            check(plain.contains("Repair requests\nEligible for repair visits.\nPool: femaleLower."),"HTML breaks stay inside their table cell");
            check(plain.contains("Literal <br> and escaped <br> and entity <br>."),"inline code, escaped tags and entities stay literal");
            check(plain.contains("<br><img src=\"never-load.png\">") && plain.contains("<!-- <br>"),"code fences and comments stay literal");
            check(plain.contains("<script>literal <br>"),"unsupported HTML remains inert literal text");
            auto bold=page->document()->find("Repair requests");
            check(bold.charFormat().fontWeight()==QFont::Bold,"HTML breaks preserve adjacent Markdown emphasis");
            auto table=bold.currentTable();
            check(table && table->rows()==2 && table->columns()==2,"HTML bridge preserves Markdown table structure");
            QVector<QSizeF> sizes;
            for(auto block=page->document()->begin();block.isValid();block=block.next())
                for(auto it=block.begin();!it.atEnd();++it)if(it.fragment().charFormat().isImageFormat()) {
                    const auto image=it.fragment().charFormat().toImageFormat(); sizes.append(QSizeF(image.width(),image.height()));
                    check(image.name()==QString::fromUtf8("wide & prüfung.png"),"HTML image attributes decode UTF-8 and entities");
                }
            check(sizes==QVector<QSizeF>{{80,40},{90,45},{60,30}},"HTML image dimensions support explicit, width-only and height-only sizes");
            check(html.metrics()["file_image_decodes"].toInt()==1,"HTML images use the bounded native resource cache");
            for(int width:{640,1000,640}) {
                html.resize(width,720); QTest::qWait(20);
                check(page->horizontalScrollBar()->maximum()==0,"mixed Markdown and HTML fits narrow cards");
            }
            const auto thumbnail=FilePreview::thumbnail(QJsonObject{{"path",htmlPath}},error);
            check(!thumbnail.isNull(),"content thumbnails share the HTML compatibility renderer");
            const auto roundingPath=temporary.filePath("table-rounding.md");
            write(roundingPath,"| One | Two | Three | Four |\n| --- | --- | --- | --- |\n| <img src='wide &amp; prüfung.png' width=320> | <img src='wide &amp; prüfung.png' width=320> | <img src='wide &amp; prüfung.png' width=320> | <img src='wide &amp; prüfung.png' width=320> |\n");
            check(html.reload(QJsonObject{{"path",roundingPath}},error),"fractional-column image table");
            for(int width:{1000,999,640,639,1000}) {
                html.resize(width,720); QTest::qWait(20);
                check(page->horizontalScrollBar()->maximum()==0,"fractional image columns do not create a one-pixel scrollbar");
                check(page->document()->documentLayout()->documentSize().width()<=page->viewport()->width(),"fitted tables stay within both odd and even viewport widths");
            }
            const auto deniedHtml=temporary.filePath("denied-html.md");
            QTemporaryDir outsideHtml;
            check(pixels.save(outsideHtml.filePath("outside.png")),"outside HTML image fixture");
#ifdef Q_OS_UNIX
            const auto pipeHtml=temporary.filePath("html-pipe.png");
            check(::mkfifo(QFile::encodeName(pipeHtml).constData(),0600)==0,"HTML pipe fixture");
#endif
            write(deniedHtml,(QString("# Restricted HTML images\n\n<img src='%1' width=40>\n\n<img src='https://example.invalid/denied.png'>\n\n<img src='html-pipe.png'>\n\n[Named target](<br>)\n").arg(outsideHtml.filePath("outside.png"))).toUtf8());
            FileView denied; denied.resize(640,500); denied.show();
            check(denied.reload(QJsonObject{{"path",deniedHtml}},error),"restricted HTML resources reload"); QTest::qWait(30);
            check(denied.metrics()["file_image_decodes"].toInt()==0,"HTML images cannot bypass local-directory, remote or regular-file boundaries");
            const auto named=denied.findChild<QTextBrowser*>("fileMarkdown")->document()->find("Named target");
            check(named.charFormat().anchorHref()=="br","HTML bridge leaves Markdown link destinations intact");
        }
        check(browser->document()->isLayoutEnabled(),"Markdown preparation restores document layout");
        check(browser->toPlainText().contains("File previews") && !browser->openExternalLinks(),"readable Markdown and inert links");
        check(view.findChild<QTextEdit*>("fileSource")->toPlainText().isEmpty(),"rendered Markdown leaves hidden source unprepared");
        check(browser->document()->defaultFont().pixelSize()==14,"Markdown uses readable preview body size");
        auto headingCursor=browser->document()->find("File previews");
        check(headingCursor.charFormat().font().pixelSize()==28,"Markdown title has a two-em heading scale");
        check(std::abs(headingCursor.blockFormat().bottomMargin()-25.4)<.1,"Markdown title reserves its rule padding and paragraph spacing");
        check(headingCursor.charFormat().fontWeight()==QFont::Bold,"Markdown title uses the browser-equivalent bold face");
        auto code=browser->document()->find("def preview");
        check(code.currentFrame()!=browser->document()->rootFrame() && code.currentFrame()->frameFormat().padding()==16,"fenced code owns a padded native panel");
        check(code.currentFrame()->frameFormat().background().color()==QColor("#2b2b2b"),"code panel follows the current theme");
        auto tableCursor=browser->document()->find("Review edits");
        auto* markdownTable=tableCursor.currentTable();
        check(markdownTable && markdownTable->format().border()==0,"Markdown tables omit heavy outer borders");
        check(markdownTable->cellAt(0,0).format().toTableCellFormat().bottomBorder()==1,"Markdown table headers have a separator");
        {
            const auto zoomPath=temporary.filePath("zoom.md");
            QImage image(80,40,QImage::Format_RGBA8888); image.fill(Qt::cyan);
            check(image.save(temporary.filePath("zoom.png")),"zoom image fixture");
            QByteArray text="# Zoom heading\n\n![local](zoom.png)\n\n```python\ndef zoom():\n    return 1\n```\n\n";
            for(int i=0;i<80;++i) text+="Paragraph "+QByteArray::number(i)+" contains **readable text** for zoom anchoring.\n\n";
            write(zoomPath,text);
            FileView zoom; zoom.resize(800,600); zoom.show(); zoom.activateWindow();
            check(zoom.reload({{"path",zoomPath}},error),"zoom Markdown reload"); QTest::qWait(40);
            auto* page=zoom.findChild<QTextBrowser*>("fileMarkdown"); page->setFocus();
            auto* originalDocument=page->document(); const auto originalText=page->toPlainText();
            const auto decodes=zoom.metrics()["file_image_decodes"].toInt();
            QTest::keyClick(page,Qt::Key_Equal,Qt::ControlModifier);
            check(zoom.metrics()["file_zoom"]==110,"Ctrl+= zooms Markdown in");
            QTest::keyClick(page,Qt::Key_Plus,Qt::ControlModifier|Qt::ShiftModifier);
            check(zoom.metrics()["file_zoom"]==120,"Ctrl++ zooms Markdown in with Shift");
            QTest::keyClick(page,Qt::Key_Plus,Qt::ControlModifier);
            check(zoom.metrics()["file_zoom"]==130,"Ctrl++ zooms Markdown in without Shift");
            QTest::keyClick(page,Qt::Key_Minus,Qt::ControlModifier);
            check(zoom.metrics()["file_zoom"]==120,"Ctrl+- zooms Markdown out");
            check(page->document()->defaultFont().pixelSize()==17,"zoom scales body text");
            check(page->document()->find("Zoom heading").charFormat().font().pixelSize()==34,"zoom scales explicit heading sizes");
            check(std::abs(page->document()->find("def zoom").currentFrame()->frameFormat().padding()-19.2)<.1,"zoom scales code-panel padding");
            auto object=page->document()->find(QString(QChar::ObjectReplacementCharacter));
            check(std::abs(object.charFormat().toImageFormat().width()-96)<.1,"zoom scales embedded images");
            auto selection=page->document()->find("Paragraph 30"); page->setTextCursor(selection); page->ensureCursorVisible();
            const auto anchor=page->cursorForPosition(QPoint(page->viewport()->width()/2,0));
            const auto top=page->cursorRect(anchor).top();
            auto wheel=[&](int angle,int pixels=0,Qt::KeyboardModifiers modifiers=Qt::ControlModifier) {
                QWheelEvent event(QPointF(100,100),page->viewport()->mapToGlobal(QPoint(100,100)),QPoint(0,pixels),QPoint(0,angle),Qt::NoButton,modifiers,Qt::NoScrollPhase,false);
                QApplication::sendEvent(page->viewport(),&event); QApplication::processEvents();
            };
            wheel(120);
            check(zoom.metrics()["file_zoom"]==130,"Ctrl+wheel zooms in");
            check(std::abs(page->cursorRect(anchor).top()-top)<=2,"zoom retains the visible reading anchor through reflow");
            check(page->textCursor().selectedText()=="Paragraph 30","zoom retains text selection");
            wheel(-60); check(zoom.metrics()["file_zoom"]==130,"partial wheel ticks accumulate");
            wheel(-60); check(zoom.metrics()["file_zoom"]==120,"accumulated Ctrl+wheel zooms out");
            wheel(0,40); check(zoom.metrics()["file_zoom"]==130,"pixel-only Ctrl+scroll zooms");
            const int scroll=page->verticalScrollBar()->value(); wheel(-120,0,Qt::NoModifier);
            check(zoom.metrics()["file_zoom"]==130 && page->verticalScrollBar()->value()>scroll,"ordinary wheel still scrolls without zooming");
            check(page->document()==originalDocument && page->toPlainText()==originalText,"zoom preserves the parsed document");
            check(zoom.metrics()["file_image_decodes"].toInt()==decodes,"zoom reuses decoded images");
            const auto evidence=qEnvironmentVariable("ASSET_PREVIEW_EVIDENCE_DIR");
            page->verticalScrollBar()->setValue(0);
            if(!evidence.isEmpty()) zoom.grab().save(evidence+"/markdown-zoom-130.png");
            auto palette=page->palette(); palette.setColor(QPalette::Text,QColor("#242424")); page->setPalette(palette);
            check(zoom.metrics()["file_zoom"]==130 && page->document()->defaultFont().pixelSize()==18,"palette changes retain zoom");
            auto* modes=zoom.findChild<QComboBox*>("fileMode");
            modes->setCurrentIndex(modes->findData("source")); modes->setCurrentIndex(modes->findData("rendered"));
            check(zoom.metrics()["file_zoom"]==130,"mode changes retain rendered zoom"); page->setFocus();
            check(zoom.reload({{"path",zoomPath}},error) && zoom.metrics()["file_zoom"]==130,"same-path reload retains zoom");
            wheel(12000); check(zoom.metrics()["file_zoom"]==300,"zoom is bounded above");
            wheel(-12000); check(zoom.metrics()["file_zoom"]==50,"zoom is bounded below");
            wheel(600);
            check(zoom.metrics()["file_zoom"]==100 && page->document()->defaultFont().pixelSize()==14,"zooming back to 100% restores baseline typography");
            check(page->document()->find("Zoom heading").charFormat().font().pixelSize()==28,"reset restores heading sizes without accumulated scaling");
            if(!evidence.isEmpty()) { page->verticalScrollBar()->setValue(0); zoom.grab().save(evidence+"/markdown-zoom-100.png"); }
        }
        view.activateWindow(); browser->setFocus(); QTest::qWait(20);
        QImage image(4,4,QImage::Format_RGB32); image.fill(Qt::red);
        QTemporaryDir outside; const auto localImage=temporary.filePath("local.png"),outsideImage=outside.filePath("outside.png");
        check(image.save(localImage) && image.save(outsideImage),"image resource fixtures");
        check(browser->document()->resource(QTextDocument::ImageResource,QUrl::fromLocalFile(localImage)).value<QImage>().pixelColor(0,0)==QColor(Qt::red),"local Markdown image");
        check(browser->document()->resource(QTextDocument::ImageResource,QUrl::fromLocalFile(outsideImage)).value<QImage>().size()==QSize(16,16),"outside image stays blocked");
        const auto link=temporary.filePath("outside-link.png"); check(QFile::link(outsideImage,link),"resource symlink fixture");
        check(browser->document()->resource(QTextDocument::ImageResource,QUrl::fromLocalFile(link)).value<QImage>().size()==QSize(16,16),"symlink cannot escape Markdown directory");
        check(browser->document()->resource(QTextDocument::ImageResource,QUrl("https://example.invalid/image.png")).value<QImage>().size()==QSize(16,16),"remote Markdown image stays blocked");
        const QByteArray svgStart="<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"23\" height=\"19\">";
        const auto svgPath=temporary.filePath("vector.svg");
        write(svgPath,"<?xml version=\"1.0\"?><!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">"+svgStart+"<style>rect { fill: red; }</style><defs><rect id=\"tile\" width=\"23\" height=\"19\"/></defs><use xlink:href=\"#tile\"/></svg>");
        if(QImageReader::supportedImageFormats().contains("svg")) {
            const auto vector=browser->document()->resource(QTextDocument::ImageResource,QUrl("vector.svg")).value<QImage>();
            check(vector.size()==QSize(23,19) && vector.pixelColor(10,10)==QColor(Qt::red),"self-contained SVG retains vector definitions, internal CSS and references");
            check(QFile::copy(svgPath,temporary.filePath("vector.png")),"SVG content with a raster extension fixture");
            const auto disguised=browser->document()->resource(QTextDocument::ImageResource,QUrl("vector.png")).value<QImage>();
            check(disguised.size()==QSize(23,19) && disguised.pixelColor(10,10)==QColor(Qt::red),"image detection recognizes SVG content despite a raster filename");
        } else std::fprintf(stdout,"SVG plugin unavailable: vector rendering check skipped\n");
        const auto svgDecodes=view.metrics()["file_image_decodes"].toInt();
        const QList<QByteArray> deniedSvg{
            svgStart+QString("<image width=\"23\" height=\"19\" xlink:href=\"%1\"/></svg>").arg(outsideImage).toUtf8(),
            svgStart+"<image width=\"23\" height=\"19\" href=\"data:image/png;base64,iVBORw0KGgo=\"/></svg>",
            svgStart+"<feImage href=\"outside.png\"/></svg>",
            "<?xml-stylesheet type=\"text/css\" href=\"outside.css\"?>"+svgStart+"<rect width=\"23\" height=\"19\"/></svg>",
            "<!DOCTYPE svg [<!ENTITY external SYSTEM \"file:///not-read\">]>"+svgStart+"<text>&external;</text></svg>",
            svgStart+"<use href=\"outside.svg#tile\"/></svg>",
            svgStart+QByteArray("<g>").repeated(129)+QByteArray("</g>").repeated(129)+"</svg>",
            svgStart+"<rect/><image href=\"missing.png\">"};
        for(int i=0;i<deniedSvg.size();++i) {
            // Content detection must cover SVG disguised as an ordinary PNG.
            const auto name=QString("denied-vector-%1.png").arg(i); write(temporary.filePath(name),deniedSvg[i]);
            check(browser->document()->resource(QTextDocument::ImageResource,QUrl(name)).value<QImage>().size()==QSize(16,16),"SVG subresources and excessive nesting cannot bypass the local image loader");
        }
        // Fixed gzip stream containing an SVG with an external raster reference.
        write(temporary.filePath("compressed.svgz"),QByteArray::fromHex("1f8b08000000000002ff75ccc10d80200c40d1554807a088270db08b094849448d54ebf8ea00defffbae5d59dd75599b0762de474411d1d2ebedc8688d31f816a0a444260fb60745a964620fdd00c1953ae5a4e848b387ede45662d2fbfa0b30b8ef171e530a386c77000000"));
        check(browser->document()->resource(QTextDocument::ImageResource,QUrl("compressed.svgz")).value<QImage>().size()==QSize(16,16),"compressed SVG cannot bypass bounded vector inspection");
        check(view.metrics()["file_image_decodes"].toInt()==svgDecodes,"denied SVG is checked before decoder sizing or rendering");
#ifdef Q_OS_UNIX
        const auto fifo=temporary.filePath("pipe.png");
        check(::mkfifo(QFile::encodeName(fifo).constData(),0600)==0,"named pipe image fixture");
        check(browser->document()->resource(QTextDocument::ImageResource,QUrl::fromLocalFile(fifo)).value<QImage>().size()==QSize(16,16),"non-regular Markdown resources cannot block the reader");
#endif
        // Exercise Qt's image handler through layout and paint as well as the
        // document API: invalid resources otherwise permit a filename retry.
        const auto deniedPath=temporary.filePath("denied.md");
        QByteArray deniedText=QString("![outside](%1)\n\n![symlink](outside-link.png)\n\n![remote](https://example.invalid/image.png)\n\n![missing](missing.png)\n\n").arg(outsideImage).toUtf8();
        deniedText+="![nested raster](denied-vector-0.png)\n\n![compressed vector](compressed.svgz)\n\n";
#ifdef Q_OS_UNIX
        write(temporary.filePath("vector-pipe.svg"),svgStart+"<image width=\"23\" height=\"19\" href=\"pipe.png\"/></svg>");
        deniedText+="![pipe](pipe.png)\n\n![nested pipe](vector-pipe.svg)\n";
#endif
        write(deniedPath,deniedText);
        const auto deniedDecodes=view.metrics()["file_image_decodes"].toInt();
        check(view.reload(QJsonObject{{"path",deniedPath}},error),"render denied Markdown resources");
        QTest::qWait(30); browser->viewport()->repaint();
        check(view.metrics()["file_image_decodes"].toInt()==deniedDecodes,"denied rendered images never reach a decoder");
        for(auto block=browser->document()->begin();block.isValid();block=block.next())
            for(auto it=block.begin();!it.atEnd();++it) {
                const auto format=it.fragment().charFormat();
                if(format.isImageFormat())
                    check(browser->document()->resource(QTextDocument::ImageResource,QUrl(format.toImageFormat().name())).value<QImage>().size()==QSize(16,16),"rendering retains the denied image placeholder");
            }
        const auto responsivePath=temporary.filePath("responsive.md");
        QImage wideImage(1600,1000,QImage::Format_RGB32); wideImage.fill(Qt::blue);
        check(wideImage.save(temporary.filePath("wide.png")),"wide Markdown image fixture");
        write(responsivePath,"![page](wide.png)\n\n> ![quote](wide.png)\n\n- ![list](wide.png)\n\n| Image | Text |\n|---|---|\n| ![table](wide.png) | Cell |\n\n![small](local.png)\n");
        check(view.reload(QJsonObject{{"path",responsivePath}},error),"responsive Markdown images");
        const auto responsiveDecodes=view.metrics()["file_image_decodes"].toInt();
        for(int width:{1280,640,960,640}) {
            view.resize(width,820); QTest::qWait(30); browser->viewport()->repaint();
            check(browser->document()->isLayoutEnabled(),"responsive image fitting restores document layout");
            check(std::abs(browser->document()->lastBlock().blockFormat().lineHeight()-std::max(0.,browser->viewport()->height()-22.4))<.1,"resize settles the final reading-space margin");
            check(browser->horizontalScrollBar()->maximum()==0,"page and nested Markdown images fit on resize");
            int imageCount=0;
            double pageWidth=0,quoteWidth=0,listWidth=0,tableWidth=0;
            for(auto block=browser->document()->begin();block.isValid();block=block.next())
                for(auto it=block.begin();!it.atEnd();++it) {
                    const auto fragment=it.fragment();
                    if(!fragment.charFormat().isImageFormat()) continue;
                    ++imageCount;
                    const auto format=fragment.charFormat().toImageFormat();
                    if(format.name()=="local.png") check(format.width()==4 && format.height()==4,"small Markdown images are never enlarged");
                    else {
                        check(std::abs(format.width()/format.height()-1.6)<.001,"responsive images preserve aspect ratio");
                        QTextCursor current(block);
                        if(current.currentTable()) tableWidth=format.width();
                        else if(block.textList()) listWidth=format.width();
                        else if(block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel)) quoteWidth=format.width();
                        else pageWidth=format.width();
                    }
                }
            check(imageCount==5 && pageWidth>0 && quoteWidth<pageWidth && listWidth<pageWidth && tableWidth<pageWidth/2,"image fitting accounts for quote, list and table containers");
        }
        check(view.metrics()["file_image_decodes"].toInt()==responsiveDecodes,"responsive image fitting reuses decoded pixels");
        write(responsivePath,"![same](wide.png)![same](wide.png)\n");
        check(view.reload(QJsonObject{{"path",responsivePath}},error),"adjacent identical Markdown images");
        QTest::qWait(30);
        auto adjacent=browser->document()->find(QString(QChar::ObjectReplacementCharacter));
        int adjacentCount=0;
        while(!adjacent.isNull()) {
            const double width=adjacent.charFormat().toImageFormat().width();
            check(width>0 && width<=browser->viewport()->width()-104,"each adjacent image fits independently");
            ++adjacentCount;
            adjacent=browser->document()->find(QString(QChar::ObjectReplacementCharacter),adjacent);
        }
        check(adjacentCount==2 && browser->horizontalScrollBar()->maximum()==0,"both adjacent images remain visible without horizontal overflow");

        // Theme changes must recolor cached Markdown without parsing it again or
        // discarding a reading selection. Qt stores explicit colors on anchors.
        const auto linksPath=temporary.filePath("links.md");
        const auto linksText=QByteArray("[Read **this guide**](guide.md)\n\n![local](local.png)\n\n")+
            QString("A paragraph keeps this image document scrollable in both card widths.\n\n").repeated(40).toUtf8();
        write(linksPath,linksText);
        QJsonObject linksConfig{{"path",linksPath}};
        check(view.reload(linksConfig,error),"Markdown link fixture"); QTest::qWait(30);
        check(browser->verticalScrollBar()->value()==0,"fresh scrollable Markdown starts at the top");
        auto guide=browser->document()->find("this guide"); browser->setTextCursor(guide);
        const auto originalMarkdown=browser->toPlainText();
        Theme::apply("light"); QTest::qWait(30);
        guide=browser->document()->find("this guide");
        check(guide.charFormat().foreground().color()==browser->palette().color(QPalette::Link),"cached Markdown links follow light palette");
        check(guide.charFormat().fontWeight()==QFont::Bold && guide.charFormat().isAnchor(),"link recoloring preserves Markdown formatting and targets");
        check(browser->textCursor().selectedText()=="this guide" && browser->toPlainText()==originalMarkdown,"link recoloring preserves text and selection");
        Theme::apply("dark"); QTest::qWait(30);
        check(browser->document()->find("this guide").charFormat().foreground().color()==browser->palette().color(QPalette::Link),"cached Markdown links follow dark palette");
        browser->document()->resource(QTextDocument::ImageResource,QUrl("local.png"));
        const auto imageDecodes=view.metrics()["file_image_decodes"].toInt();
        for(int i=0;i<6;++i) {
            browser->document()->resource(QTextDocument::ImageResource,QUrl("local.png"));
            view.resize(i%2?1280:640,820); QTest::qWait(10); browser->viewport()->repaint();
            check(browser->verticalScrollBar()->maximum()>0,"cache regression fixture is actually scrollable");
            browser->verticalScrollBar()->setValue(i%2?browser->verticalScrollBar()->maximum():0);
        }
        check(view.metrics()["file_image_decodes"].toInt()==imageDecodes,"scroll, resize and resource queries reuse decoded Markdown images");
        write(linksPath,QByteArray("\0",1));
        check(!view.reload(linksConfig,error),"reject an invalid image document save");
        browser->document()->resource(QTextDocument::ImageResource,QUrl("local.png"));
        check(view.metrics()["file_image_decodes"].toInt()==imageDecodes && browser->toPlainText()==originalMarkdown,"rejected saves preserve the last-good image cache and document");
        write(linksPath,linksText);
        QImage bounded(1024,1024,QImage::Format_RGB32); bounded.fill(Qt::blue);
        check(bounded.save(temporary.filePath("bounded.png")),"cache budget image fixture");
        for(int i=0;i<17;++i) {
            const auto url=QUrl(QString("bounded.png?copy=%1").arg(i));
            check(browser->document()->resource(QTextDocument::ImageResource,url).value<QImage>().pixelColor(0,0)==QColor(Qt::blue),"bounded cache retains readable resources");
        }
        check(view.metrics()["file_image_cache_cost"].toInt()<=64*1024*1024,"Markdown image cache has a finite memory budget");
        const auto evictedDecodes=view.metrics()["file_image_decodes"].toInt();
        browser->document()->resource(QTextDocument::ImageResource,QUrl("bounded.png?copy=0"));
        check(view.metrics()["file_image_decodes"].toInt()==evictedDecodes+1,"evicted Markdown images reload on demand");
        {
            // A document larger than the pixel budget must not decode every
            // offscreen image again just to recompute its displayed rectangle.
            const auto heavyPath=temporary.filePath("image-heavy.md");
            QByteArray heavyText="# Image-heavy guide\n\n";
            for(int i=0;i<17;++i) heavyText+=QString("![screen](bounded.png?copy=%1)\n\n").arg(i).toUtf8();
            write(heavyPath,heavyText);
            FileView heavy; heavy.resize(1000,680); heavy.show();
            check(heavy.reload(QJsonObject{{"path",heavyPath}},error),"over-budget Markdown image guide");
            auto* pane=heavy.findChild<QTextBrowser*>("fileMarkdown");
            QTest::qWait(30); pane->verticalScrollBar()->setValue(0);
            heavy.resize(640,680); QTest::qWait(30); pane->viewport()->repaint();
            const auto readyDecodes=heavy.metrics()["file_image_decodes"].toInt();
            for(int i=0;i<8;++i) {
                heavy.resize(i%2?640:1000,680); QTest::qWait(5); pane->viewport()->repaint();
                check(pane->horizontalScrollBar()->maximum()==0,"over-budget image guide stays within the viewport");
            }
            check(heavy.metrics()["file_image_decodes"].toInt()==readyDecodes,"resize retains intrinsic sizes after pixel-cache eviction");
            check(heavy.metrics()["file_image_cache_cost"].toInt()<=64*1024*1024,"intrinsic sizes do not retain evicted pixels");
            QImage replacement(512,1024,QImage::Format_RGB32); replacement.fill(Qt::yellow);
            check(replacement.save(temporary.filePath("bounded.png")),"replace over-budget guide images");
            check(heavy.reload(QJsonObject{{"path",heavyPath}},error),"same-text over-budget image refresh");
            QTest::qWait(30);
            for(auto block=pane->document()->begin();block.isValid();block=block.next())
                for(auto it=block.begin();!it.atEnd();++it) if(it.fragment().charFormat().isImageFormat()) {
                    const auto format=it.fragment().charFormat().toImageFormat();
                    check(std::abs(format.width()/format.height()-.5)<.001,"same-text refresh invalidates retained intrinsic sizes");
                }
        }
        // A same-text reload still refreshes the bounded local image loader.
        browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum()/2);
        const auto imageReadingPosition=browser->verticalScrollBar()->value();
        const auto oldImageLayoutHeight=browser->document()->size().height();
        image=QImage(8,200,QImage::Format_RGB32); image.fill(Qt::green); check(image.save(localImage),"replace Markdown image");
        check(view.reload(linksConfig,error),"same-text Markdown reload");
        check(browser->document()->resource(QTextDocument::ImageResource,QUrl("local.png")).value<QImage>().pixelColor(0,0)==QColor(Qt::green),"same-text reload refreshes local images");
        check(browser->document()->resource(QTextDocument::ImageResource,QUrl("local.png")).value<QImage>().size()==QSize(8,200),"same-text reload refreshes image dimensions");
        check(browser->document()->size().height()>oldImageLayoutHeight+100,"same-text image growth updates rendered document layout");
        QTest::qWait(30);
        check(browser->verticalScrollBar()->value()==imageReadingPosition,"same-text image growth preserves the reading position");
        const auto readingSelection=browser->document()->find("this guide"); browser->setTextCursor(readingSelection);
        browser->verticalScrollBar()->setValue(browser->verticalScrollBar()->maximum()/2);
        const auto textReadingPosition=browser->verticalScrollBar()->value();
        write(linksPath,linksText+"\nAn updated final paragraph.\n");
        check(view.reload(linksConfig,error),"refresh Markdown text while reading"); QTest::qWait(30);
        check(browser->textCursor().selectedText()=="this guide","Markdown text refresh preserves the reading selection");
        check(browser->verticalScrollBar()->value()==textReadingPosition,"Markdown text refresh preserves the reading position");
        check(view.reload(config,error),"restore Markdown fixture"); QTest::qWait(30);
        const QString evidence=qEnvironmentVariable("ASSET_PREVIEW_EVIDENCE_DIR");
        if(!evidence.isEmpty()) { QDir().mkpath(evidence); view.grab().save(evidence+"/markdown.png"); }
        // Exercise the rendered document itself at both themes and card widths,
        // including repeated restyling of cached code frames and inline code.
        const auto styledPath=temporary.filePath("styled.md");
        write(styledPath,"# Markdown preview\n\nReadable paragraphs with **bold**, *emphasis*, `inline code` and [a local guide](guide.md).\n\n## Code and quotations\n\n> A quoted paragraph with **emphasis**.\n>\n> A second paragraph in the same quote.\n\n```python\ndef preview(path):\n    # A deliberately long explanation demonstrates how code stays readable when the preview is resized into a narrow document card.\n    return {\"kind\": \"file\", \"watch\": True}\n```\n\n| View | Use |\n|---|---|\n| Rendered | Read documents |\n| Source | Inspect Markdown |\n\n### Lists\n\n- One item\n  - Nested item\n- Another item\n\n1. Read\n2. Compare\n\nIndented example.\n\n    indented code\n\nA final paragraph.\n");
        check(view.reload(QJsonObject{{"path",styledPath}},error),"styled Markdown fixture");
        if(!evidence.isEmpty()) write(evidence+"/styled.html",browser->document()->toHtml().toUtf8());
        auto styledText=browser->toPlainText();
        auto selected=browser->document()->find("inline code"); browser->setTextCursor(selected);
        for(const auto& theme:{"dark","light"}) {
            Theme::apply(theme); QTest::qWait(30);
            check(browser->toPlainText()==styledText && browser->textCursor().selectedText()=="inline code","Markdown restyling preserves cached text and selection");
            check(browser->document()->find("inline code").charFormat().background().color()==QColor(QString(theme)=="dark"?"#3c3c3c":"#e0e0e0"),"inline code recolors with the theme");
            auto panel=browser->document()->find("def preview").currentFrame();
            check(panel->frameFormat().background().color()==QColor(QString(theme)=="dark"?"#2b2b2b":"#f8f8f8"),"cached code panel recolors with the theme");
            auto indented=browser->document()->find("indented code");
            check(indented.currentFrame()!=browser->document()->rootFrame(),"indented code also has a native panel");
            auto quote=browser->document()->find("A quoted paragraph");
            check(quote.blockFormat().leftMargin()==15 && quote.blockFormat().intProperty(QTextFormat::BlockQuoteLevel)==1,"quote styling preserves semantic indentation");
            for(int width:{1280,640}) {
                view.resize(width,820); QTest::qWait(40);
                check(browser->horizontalScrollBar()->maximum()==0,"rendered Markdown fits desktop and narrow cards");
                const auto firstHeader=browser->document()->find("View",0,QTextDocument::FindCaseSensitively),secondHeader=browser->document()->find("Use",0,QTextDocument::FindCaseSensitively);
                const auto firstRect=browser->document()->documentLayout()->blockBoundingRect(firstHeader.block());
                const auto secondRect=browser->document()->documentLayout()->blockBoundingRect(secondHeader.block());
                check(std::abs(firstRect.top()-secondRect.top())<1,"Markdown table header cells share a row after a code panel");
                if(!evidence.isEmpty()) view.grab().save(evidence+QString("/markdown-%1-%2.png").arg(theme).arg(width));
            }
        }
        const auto literalPath=temporary.filePath("literal-fences.md");
        write(literalPath,"> Before the embedded fence.\n>\n> ```python\n> def quoted(value):\n>     return value + 1\n> ```\n>\n> After the embedded fence.\n\n## Following heading\n\n| Column | Use |\n|---|---|\n| Entry | Reading |\n\n```cpp\nreturn 42;\n```\n\n```text\nreturn 123 # literal text\n```\n\n```unknown-language\nreturn 456 # unknown language\n```\n\n```\nreturn 789 # unlabelled\n```\n");
        check(view.reload(QJsonObject{{"path",literalPath}},error),"literal and labelled Markdown fences");
        for(const auto& theme:{"dark","light"}) {
            Theme::apply(theme); QTest::qWait(30);
            const auto quotedCode=browser->document()->find("def quoted");
            check(quotedCode.currentFrame()->frameFormat().leftMargin()==15 && quotedCode.currentFrame()->frameFormat().rightMargin()==16,"quoted code panel retains the quote inset");
            check(browser->document()->find("Before the embedded fence.").blockFormat().bottomMargin()==16,"quote paragraph spacing survives an embedded code panel");
            const auto quoteEnd=browser->document()->documentLayout()->blockBoundingRect(browser->document()->find("After the embedded fence.").block());
            const auto followingHeading=browser->document()->documentLayout()->blockBoundingRect(browser->document()->find("Following heading").block());
            const auto columnHeader=browser->document()->documentLayout()->blockBoundingRect(browser->document()->find("Column").block());
            check(std::abs(followingHeading.top()-quoteEnd.top()-22.4-24)<1,"a heading after a quote has one collapsed section gap");
            check(std::abs(columnHeader.top()-followingHeading.top()-54.55)<1,"tables preserve heading rule padding and paragraph spacing");
            check(!browser->document()->find("return 42;").block().layout()->formats().isEmpty(),"supported fences retain basic syntax colors");
            for(const auto* text:{"return 123 # literal text","return 456 # unknown language","return 789 # unlabelled"}) {
                const auto literal=browser->document()->find(text);
                check(!literal.isNull() && literal.block().layout()->formats().isEmpty(),"literal and unknown fences do not invent syntax colors");
            }
        }
        Theme::apply("dark"); view.resize(1280,820);
        check(view.reload(config,error),"restore Markdown after presentation checks"); QTest::qWait(30);
        // The separate heavy-image fixture took focus before it closed.
        view.activateWindow(); QTest::qWait(20);
        mode->setCurrentIndex(mode->findData("source"));
        auto* source=view.findChild<QTextEdit*>("fileSource");
        check(source->isReadOnly() && source->isVisible(),"read-only source toggle");
        auto* wrap=view.findChild<QCheckBox*>("fileWrap");
        check(wrap->isChecked() && source->lineWrapMode()==QTextEdit::WidgetWidth,"source defaults to soft wrap");
        check(source->y()<45,"file content begins below one compact toolbar");
        auto* fileSummary=view.findChild<QLabel*>("fileSummary");
        for(int width:{1280,720,640}) {
            view.resize(width,820); QTest::qWait(20);
            check(fileSummary->isVisible() && fileSummary->width()>40,"ordinary filename remains visible in narrow previews");
            check(!fileSummary->text().isEmpty(),"ordinary file summary retains its name");
        }
        view.resize(1280,820); QTest::qWait(20);
        source->setFocus(); QTest::keyClick(source,Qt::Key_Z,Qt::AltModifier);
        check(!wrap->isChecked() && source->lineWrapMode()==QTextEdit::NoWrap,"Alt+Z disables soft wrap");
        QTest::keyClick(source,Qt::Key_Z,Qt::AltModifier);
        check(wrap->isChecked(),"Alt+Z enables soft wrap");
        source->setFocus(); QTest::keyClick(source,Qt::Key_F,Qt::ControlModifier);
        auto* search=view.findChild<QLineEdit*>("fileSearch");
        check(search->hasFocus(),"Ctrl+F focuses find");
        search->setText("compare changes"); QTest::keyClick(search,Qt::Key_Return);
        check(source->textCursor().selectedText()=="compare changes","find selects exact match");
        QSignalSpy sourceChanges(source->document(),&QTextDocument::contentsChanged);
        QSignalSpy modeChanges(mode,&QComboBox::currentIndexChanged);
        wrap->click(); wrap->click();
        check(sourceChanges.isEmpty() && modeChanges.isEmpty(),"soft wrap retains the document and mode selector");
        check(source->textCursor().selectedText()=="compare changes","soft wrap preserves selected text");
        const auto old=source->toPlainText(); write(path,QByteArray("\0",1));
        check(!view.reload(config,error) && source->toPlainText()==old,"last-good content survives bad saves");
        const auto combinedFile=temporary.filePath("merge.diff"); write(combinedFile,combined.toUtf8());
        QJsonObject combinedConfig{{"path",combinedFile},{"settings",QJsonObject{{"file_mode","inline"}}}};
        check(view.reload(combinedConfig,error),"combined patch view");
        check(view.metrics()["file_mode"]=="source" && source->toPlainText()==combined,"combined patch preserves all merge columns in source mode");
        const auto combinedMarkdown=temporary.filePath("merge.md"); write(combinedMarkdown,combined.toUtf8());
        combinedConfig["path"]=combinedMarkdown;
        check(view.reload(combinedConfig,error) && mode->count()==1 && view.metrics()["file_mode"]=="source","Git combined content falls back to source even with a Markdown extension");
        const auto patchFile=temporary.filePath("changes.diff"); write(patchFile,patch.toUtf8()); config["path"]=patchFile;
        config.remove("settings");
        check(view.reload(config,error),"patch view"); QTest::qWait(80);
        check(view.metrics()["file_mode"]=="inline","patch defaults to inline after a source document");
        check(view.findChild<QTextEdit*>("fileBefore")->toPlainText().isEmpty() && view.findChild<QTextEdit*>("fileAfter")->toPlainText().isEmpty(),"inline preview leaves hidden split panes unprepared");
        auto* summary=view.findChild<QLabel*>("fileSummary");
        auto countColors=[&] {
            QTextDocument label; label.setHtml(summary->text());
            const auto added=label.find("+").charFormat().foreground().color();
            const auto removed=label.find("-").charFormat().foreground().color();
            check(added.hue()>=90 && added.hue()<=160,"addition count is green");
            check(removed.hue()>=340 || (removed.hue()>=0 && removed.hue()<=20),"deletion count is red");
        };
        countColors();
        for(auto* button:view.findChildren<QToolButton*>())
            check(button->toolTip()!="Previous change" && button->toolTip()!="Next change","change arrows are absent from the toolbar");
        QMenu inlineMenu;
        view.addNavigationActions(&inlineMenu);
        check(inlineMenu.actions().size()==2,"inline change navigation remains in the menu");
        auto* tree=view.findChild<QTreeWidget*>("changedFiles");
        check(tree->topLevelItemCount()==2 && tree->isVisible(),"changed-file navigation");
        if(!evidence.isEmpty()) view.grab().save(evidence+"/inline.png");
        tree->setCurrentItem(tree->topLevelItem(1)); check(view.metrics()["selected_file"].toInt()==1,"select changed file");
        tree->setCurrentItem(tree->topLevelItem(0));
        mode->setCurrentIndex(mode->findData("split")); QTest::qWait(80);
        check(view.findChild<QTextEdit*>("fileBefore")->isVisible() && view.findChild<QTextEdit*>("fileAfter")->isVisible(),"split panes");
        if(!evidence.isEmpty()) view.grab().save(evidence+"/split.png");
        view.resize(640,700); QTest::qWait(80);
        check(!tree->isVisible() && view.width()==640,"narrow view retains content width");
        auto* picker=view.findChild<QComboBox*>("changedFilePicker");
        check(picker->isVisible() && picker->count()==2,"narrow changed-file navigation remains available");
        check(actions->mapTo(&view,actions->rect().bottomRight()).x()<view.width(),"narrow card actions stay inside the toolbar");
        check(search->mapTo(&view,search->rect().bottomRight()).x()<view.width(),"narrow find controls remain reachable");
        picker->setCurrentIndex(1); check(view.metrics()["selected_file"].toInt()==1,"narrow file selection");
        picker->setCurrentIndex(0);
        if(!evidence.isEmpty()) view.grab().save(evidence+"/narrow.png");
        Theme::apply("light"); QTest::qWait(80);
        countColors();
        for(const auto& name:{"fileInline","fileBefore","fileAfter"}) {
            const auto highlights=view.findChild<QTextEdit*>(name)->extraSelections();
            check(!highlights.isEmpty(),"diff highlights remain after a theme switch");
            for(const auto& highlight:highlights)
                check(highlight.format.background().color().lightness()>128,"theme switch recolors visible and cached diff rows");
        }
        if(!evidence.isEmpty()) view.grab().save(evidence+"/light.png");
        QString a,b;
        for(int i=1;i<=180;++i) { a+=QString("line %1\n").arg(i); if(i!=20 && i!=160) b+=QString("line %1\n").arg(i); }
        const auto longAfter=temporary.filePath("after.cpp"); write(before,a.toUtf8()); write(longAfter,b.toUtf8());
        config=QJsonObject{{"path",longAfter},{"before",before},{"settings",QJsonObject{{"file_mode","split"}}}};
        check(view.reload(config,error),"long deletion-only diff");
        auto* left=view.findChild<QTextEdit*>("fileBefore"); auto* right=view.findChild<QTextEdit*>("fileAfter");
        check(left->toPlainText().contains("unchanged lines"),"long context collapses");
        view.findChild<QCheckBox*>("fileContext")->setChecked(true); QTest::qWait(30);
        check(!left->toPlainText().contains("unchanged lines"),"full context expands");
        QMenu changeMenu;
        view.addNavigationActions(&changeMenu);
        check(changeMenu.actions().size()==2,"split change navigation remains in the menu");
        auto* previousChange=changeMenu.actions().at(0); auto* nextChange=changeMenu.actions().at(1);
        check(previousChange->text()=="Previous change" && nextChange->text()=="Next change","change menu labels explain the actions");
        nextChange->trigger(); check(right->textCursor().blockNumber()==19,"split navigation finds deletion-only changes");
        nextChange->trigger(); check(right->textCursor().blockNumber()==159,"next distinct hunk");
        previousChange->trigger(); check(right->textCursor().blockNumber()==19,"previous change menu navigation");
        previousChange->trigger(); check(right->textCursor().blockNumber()==159,"previous change wraps to the last hunk");
        left->verticalScrollBar()->setValue(70);
        check(right->verticalScrollBar()->value()==left->verticalScrollBar()->value(),"paired scrolling");
        view.findChild<QCheckBox*>("fileWrap")->toggle(); QTest::qWait(30);
        check(left->verticalScrollBar()->value()==70,"settings retain reading position");
        left->setFocus(); QTest::keyClick(left,Qt::Key_F,Qt::ControlModifier);
        search->setText("line 160"); QTest::keyClick(search,Qt::Key_Return);
        check(left->textCursor().selectedText()=="line 160","find retains the before pane after focusing search");
        // Asymmetric replacements must not offset following rows when either
        // side wraps. Text copied out of the document stays exact.
        const QString longLine=QString("long_token ").repeated(50);
        write(before,(longLine+"\nshared row\n").toUtf8());
        write(longAfter,"short replacement\nshared row\n");
        config["settings"]=QJsonObject{{"file_mode","split"},{"file_context",true}};
        check(view.reload(config,error),"wrapped replacement reload"); QTest::qWait(80);
        check(wrap->isEnabled() && wrap->isChecked() && left->lineWrapMode()==QTextEdit::WidgetWidth,"split defaults to enabled soft wrap");
        check(left->document()->begin().layout()->lineCount()>1,"long split row actually wraps");
        auto aligned=[&] {
            const auto a=left->document()->documentLayout()->blockBoundingRect(left->document()->begin().next());
            const auto b=right->document()->documentLayout()->blockBoundingRect(right->document()->begin().next());
            return qAbs(a.top()-b.top())<1;
        };
        check(aligned(),"wrapped replacement keeps following paired rows aligned");
        check(left->toPlainText()==longLine+"\nshared row","alignment does not add copied padding");
        left->setFocus(); QTest::keyClick(left,Qt::Key_Z,Qt::AltModifier); QTest::qWait(40);
        check(!wrap->isChecked() && left->lineWrapMode()==QTextEdit::NoWrap && aligned(),"Alt+Z disables split wrapping and removes alignment padding");
        QTest::keyClick(left,Qt::Key_Z,Qt::AltModifier); QTest::qWait(40);
        view.resize(1050,720); QTest::qWait(80);
        check(aligned(),"paired soft wrap realigns after resize");
        const auto originalFont=left->font(); auto largerFont=originalFont;
        largerFont.setPointSizeF(originalFont.pointSizeF()+2); left->setFont(largerFont); QTest::qWait(50);
        check(aligned(),"paired rows realign after a font change");
        left->setFont(originalFont); QTest::qWait(50);
        if(!evidence.isEmpty()) view.grab().save(evidence+"/wrapped-split.png");
        // Returning to a cached mode must honor context, file selection and a
        // same-path, same-size reload. Hidden panes cannot serve stale rows.
        const auto shared=QString("same\n").repeated(30);
        write(before,("old\n"+shared).toUtf8()); write(longAfter,("new\n"+shared).toUtf8());
        config["settings"]=QJsonObject{{"file_mode","inline"}};
        check(view.reload(config,error),"cached-mode fixture");
        auto* inlinePane=view.findChild<QTextEdit*>("fileInline");
        check(inlinePane->toPlainText().contains("unchanged lines"),"inline context starts collapsed");
        mode->setCurrentIndex(mode->findData("split"));
        check(left->toPlainText().contains("unchanged lines"),"split mode prepares collapsed rows");
        mode->setCurrentIndex(mode->findData("inline"));
        view.findChild<QCheckBox*>("fileContext")->setChecked(true);
        mode->setCurrentIndex(mode->findData("split")); QTest::qWait(40);
        check(!left->toPlainText().contains("unchanged lines"),"context invalidates a hidden split page");
        mode->setCurrentIndex(mode->findData("inline"));
        write(longAfter,("NEW\n"+shared).toUtf8());
        check(view.reload(config,error),"same-size input replacement");
        check(inlinePane->toPlainText().contains("NEW"),"reload refreshes the visible page");
        mode->setCurrentIndex(mode->findData("split"));
        check(right->toPlainText().contains("NEW") && !right->toPlainText().contains("new"),"reload invalidates a hidden page");
        const auto escaped=temporary.filePath("name<&>.txt"); write(escaped,"plain source\n");
        config=QJsonObject{{"path",escaped}};
        check(view.reload(config,error),"literal filename fixture");
        QTextDocument title; title.setHtml(summary->text());
        check(title.toPlainText()=="name<&>.txt","count markup keeps filenames literal");
        const auto python=temporary.filePath("same.py"),cpp=temporary.filePath("same.cpp");
        write(python,"# comment\n"); write(cpp,"# comment\n");
        config=QJsonObject{{"path",python},{"settings",QJsonObject{{"file_mode","source"}}}};
        check(view.reload(config,error),"Python syntax fixture"); QTest::qWait(30);
        check(!source->document()->begin().layout()->formats().isEmpty(),"first preparation highlights source automatically");
        config["path"]=cpp;
        check(view.reload(config,error),"same text with different language"); QTest::qWait(30);
        check(source->document()->begin().layout()->formats().isEmpty(),"language change invalidates syntax for identical text");
        {
            QImage pixels(320,160,QImage::Format_ARGB32); pixels.fill(QColor(30,90,180,127));
            pixels.setPixelColor(0,0,Qt::transparent); pixels.setPixelColor(319,159,QColor(210,60,40,255));
            check(pixels.save(temporary.filePath("clipboard.png")),"clipboard alpha fixture");
            const auto copyPath=temporary.filePath("clipboard.md");
            write(copyPath,"# Copy images\n\nbefore ![inline](clipboard.png) after\n\n| Picture | Label |\n|---|---|\n| ![table](clipboard.png) | adjacent text |\n");
            FileView copyView; copyView.resize(360,400); copyView.show();
            check(copyView.reload(QJsonObject{{"path",copyPath}},error),"clipboard Markdown reload"); QTest::qWait(40);
            auto* content=copyView.findChild<QTextBrowser*>("fileMarkdown");
            QVector<int> positions;
            for(auto block=content->document()->begin();block.isValid();block=block.next())
                for(auto it=block.begin();!it.atEnd();++it)if(it.fragment().charFormat().isImageFormat())positions.append(it.fragment().position());
            check(positions.size()==2,"inline and table clipboard images exist");
            const auto verifyClipboard=[&] {
                const auto* mime=QApplication::clipboard()->mimeData();
                check(mime->hasImage() && mime->hasFormat("image/png"),"embedded image copy publishes image and PNG MIME types");
                const auto copied=QApplication::clipboard()->image();
                check(copied.size()==pixels.size() && copied.convertToFormat(QImage::Format_RGBA8888)==pixels.convertToFormat(QImage::Format_RGBA8888),"clipboard keeps decoded dimensions and exact alpha pixels");
                check(QImage::fromData(mime->data("image/png"),"PNG").convertToFormat(QImage::Format_RGBA8888)==pixels.convertToFormat(QImage::Format_RGBA8888),"PNG paste payload preserves image pixels");
            };
            const auto decodes=copyView.metrics()["file_image_decodes"].toInt();
            for(int position:positions) for(double fraction:{.25,.75}) {
                QTextCursor imageCursor(content->document()); imageCursor.setPosition(position);
                content->setTextCursor(imageCursor); content->ensureCursorVisible(); QTest::qWait(20);
                const auto caret=content->cursorRect(imageCursor);
                imageCursor.movePosition(QTextCursor::NextCharacter,QTextCursor::KeepAnchor);
                const auto format=imageCursor.charFormat().toImageFormat();
                check(format.width()<pixels.width(),"clipboard fixture is displayed smaller than source pixels");
                const QPoint point(caret.left()+int(format.width()*fraction),caret.bottom()-int(format.height()/2));
                QTest::mouseClick(content->viewport(),Qt::LeftButton,Qt::NoModifier,point);
                check(content->textCursor().hasSelection() && content->textCursor().charFormat().isImageFormat(),"either half of an image selects that image");
                QTest::keyClick(content,Qt::Key_C,Qt::ControlModifier); verifyClipboard();
                QApplication::clipboard()->clear();
                bool offered=false;
                QTimer::singleShot(0,[&] {
                    if(auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                        if(auto* action=menu->findChild<QAction*>("copyMarkdownImage");action && action->isEnabled()) {
                            const auto evidence=qEnvironmentVariable("ASSET_PREVIEW_EVIDENCE_DIR");
                            if(!evidence.isEmpty()) { QDir().mkpath(evidence); menu->grab().save(evidence+"/markdown-image-copy-menu.png"); }
                            offered=true; QTest::mouseClick(menu,Qt::LeftButton,Qt::NoModifier,menu->actionGeometry(action).center());
                        } else menu->close();
                    }
                });
                QContextMenuEvent contextEvent(QContextMenuEvent::Mouse,point,content->viewport()->mapToGlobal(point));
                QApplication::sendEvent(content->viewport(),&contextEvent);
                check(offered,"image context menu offers Copy Image"); verifyClipboard();
            }
            check(copyView.metrics()["file_image_decodes"].toInt()==decodes,"copy reuses already decoded image pixels");
            content->setTextCursor(content->document()->find("before")); content->copy();
            check(QApplication::clipboard()->text()=="before" && !QApplication::clipboard()->mimeData()->hasImage(),"normal Markdown text copying remains text");
            QTextCursor mixed(content->document()); mixed.select(QTextCursor::Document); content->setTextCursor(mixed); content->copy();
            check(QApplication::clipboard()->text().contains("before") && !QApplication::clipboard()->mimeData()->hasImage(),"mixed text and image selections retain standard document copying");
            write(copyPath,"![denied](https://example.invalid/image.png)\n");
            check(copyView.reload(QJsonObject{{"path",copyPath}},error),"denied clipboard image fixture");
            QTextCursor denied(content->document()); denied.movePosition(QTextCursor::NextCharacter,QTextCursor::KeepAnchor); content->setTextCursor(denied); content->copy();
            check(!QApplication::clipboard()->mimeData()->hasImage(),"denied image placeholder is not copied as actual image pixels");
        }
        if(qEnvironmentVariableIsSet("ASSET_PREVIEW_GPU_TEST")) {
            QOpenGLContext context; check(context.create(),"OpenGL context"); QOffscreenSurface surface; surface.setFormat(context.format()); surface.create();
            check(context.makeCurrent(&surface),"OpenGL hardware proof");
            const QString renderer=QString::fromLatin1(reinterpret_cast<const char*>(context.functions()->glGetString(GL_RENDERER)));
            check(!renderer.isEmpty() && !renderer.contains("llvmpipe",Qt::CaseInsensitive) && !renderer.contains("softpipe",Qt::CaseInsensitive),"hardware renderer required");
            std::fprintf(stdout,"File preview hardware renderer: %s\n",qPrintable(renderer));
            if(!evidence.isEmpty()) write(evidence+"/renderer.json",QJsonDocument(QJsonObject{{"renderer",renderer},{"qt_platform",QGuiApplication::platformName()}}).toJson());
            context.doneCurrent();
        }
        std::fprintf(stdout,"File preview: 500 reconstruction cases, bounded rewrites, patch offsets, resource boundaries, content limits and native view checks passed\n");
    } catch(const std::exception& error) { std::fprintf(stderr,"File preview check failed: %s\n",error.what()); return 1; }
    return 0;
}
