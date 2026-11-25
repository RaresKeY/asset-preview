#include "file_view.h"
#include "markdown_compat.h"
#include <QApplication>
#include <QBuffer>
#include <QCache>
#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QTextEdit>
#include <QAbstractTextDocumentLayout>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStylePainter>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QTextFrame>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextTable>
#include <QPainterPath>
#include <QSet>
#include <QToolButton>
#include <QTreeWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <memory>

namespace {
bool dark(const QWidget* widget) { return widget->palette().color(QPalette::Base).lightness()<128; }
QColor countColor(const QPalette& palette,bool added,bool selected=false) {
    if(selected && palette.color(QPalette::Highlight).lightness()<128) return QColor(added?"#b4f0bd":"#ffccc7");
    const bool d=palette.color(selected?QPalette::Highlight:QPalette::Base).lightness()<128;
    return QColor(added?(d?"#7acb8e":"#216e39"):(d?"#ef918c":"#b4232c"));
}
enum ChangeRole { ChangeName=Qt::UserRole+1,AddedCount,RemovedCount };
class ChangeDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index) const override {
        if(!index.data(ChangeName).isValid()) { QStyledItemDelegate::paint(painter,option,index); return; }
        auto opt=option; initStyleOption(&opt,index); opt.text.clear();
        auto* style=opt.widget?opt.widget->style():QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem,&opt,painter,opt.widget);
        auto rect=style->subElementRect(QStyle::SE_ItemViewItemText,&opt,opt.widget).adjusted(4,0,-4,0);
        const auto added=QString("+%1").arg(index.data(AddedCount).toInt()),removed=QString("-%1").arg(index.data(RemovedCount).toInt());
        const int gap=opt.fontMetrics.horizontalAdvance(' ')*2;
        const int aw=opt.fontMetrics.horizontalAdvance(added),rw=opt.fontMetrics.horizontalAdvance(removed);
        painter->save(); painter->setFont(opt.font); painter->setClipRect(rect);
        painter->setPen(opt.palette.color(opt.state & QStyle::State_Selected?QPalette::HighlightedText:QPalette::Text));
        painter->drawText(rect.adjusted(0,0,-aw-rw-2*gap,0),Qt::AlignVCenter|Qt::AlignLeft,
            opt.fontMetrics.elidedText(index.data(ChangeName).toString(),Qt::ElideMiddle,std::max(0,rect.width()-aw-rw-2*gap)));
        painter->setPen(countColor(opt.palette,true,opt.state & QStyle::State_Selected));
        painter->drawText(rect.adjusted(rect.width()-aw-rw-gap,0,-rw-gap,0),Qt::AlignVCenter|Qt::AlignRight,added);
        painter->setPen(countColor(opt.palette,false,opt.state & QStyle::State_Selected));
        painter->drawText(rect.adjusted(rect.width()-rw,0,0,0),Qt::AlignVCenter|Qt::AlignRight,removed);
        painter->restore();
    }
};
class ChangePicker final : public QComboBox {
public:
    using QComboBox::QComboBox;
protected:
    void paintEvent(QPaintEvent*) override {
        QStylePainter painter(this); QStyleOptionComboBox opt; initStyleOption(&opt);
        painter.drawComplexControl(QStyle::CC_ComboBox,opt);
        QStyleOptionViewItem label; label.initFrom(this); label.widget=this; label.font=font();
        label.rect=style()->subControlRect(QStyle::CC_ComboBox,&opt,QStyle::SC_ComboBoxEditField,this);
        label.state&=~(QStyle::State_HasFocus|QStyle::State_MouseOver);
        itemDelegate()->paint(&painter,label,model()->index(currentIndex(),modelColumn(),rootModelIndex()));
    }
};
QColor tint(const QWidget* widget,QChar kind,bool strong=false) {
    const bool d=dark(widget);
    if (kind=='+') return QColor(d?(strong?"#335b3c":"#203b29"):(strong?"#a9dbb8":"#e2f3e7"));
    if (kind=='-') return QColor(d?(strong?"#78423e":"#492a2b"):(strong?"#efb5af":"#fbe5e3"));
    return QColor(d?"#282d34":"#e8edf2");
}
bool markdownCode(const QTextBlock& block) {
    if(!block.isValid()) return false;
    if(block.blockFormat().hasProperty(QTextFormat::BlockCodeFence) || block.blockFormat().nonBreakableLines()) return true;
    // Qt represents indented code as a whole monospace paragraph, without a fence.
    const auto first=block.begin();
    return !first.atEnd() && first.fragment().length()==block.length()-1 &&
        first.fragment().charFormat().fontFamilies().toStringList().contains("monospace") &&
        !first.fragment().charFormat().fontFixedPitch();
}
class Syntax final : public QSyntaxHighlighter {
public:
    Syntax(QTextDocument* document,QWidget* owner,bool fences=false):QSyntaxHighlighter(document),owner(owner),fences(fences) {}
    bool language(const QString& value) {
        if(extension==value) return false;
        extension=value;
        hash=QStringList{"py","pyi","gd","gdshader","sh","bash","fish","zsh","yaml","yml","toml","ini","cfg","conf","cmake","env"}.contains(extension);
        markdown=QStringList{"md","markdown","mdown"}.contains(extension);
        return true;
    }
protected:
    void highlightBlock(const QString& text) override {
        if(fences) {
            if(!markdownCode(currentBlock())) return;
            auto lang=currentBlock().blockFormat().stringProperty(QTextFormat::BlockCodeLanguage).toLower();
            if(lang=="python") lang="py";
            if(lang=="shell") lang="sh";
            // Unlabelled, text and unknown fences are literal examples. The
            // basic lexer must not invent keyword colors for their contents.
            static const QStringList supported{"c","cc","cpp","c++","h","hpp","cs","csharp","java","js","javascript","jsx",
                "ts","typescript","tsx","json","py","pyi","gd","gdscript","gdshader","glsl","hlsl","sh","bash","fish","zsh",
                "yaml","yml","toml","ini","cfg","conf","cmake","env","rs","rust"};
            if(!supported.contains(lang)) return;
            language(lang);
        }
        const bool d=dark(owner);
        static const QRegularExpression tokens(
            R"re(("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`[^`]*`)|\b(true|false|null|None|True|False|if|else|elif|for|while|return|class|struct|enum|def|func|var|const|let|auto|int|float|double|bool|void|public|private|protected|import|from|include|extends|namespace|using|static|virtual|override|export|async|await|try|except|catch|throw|new|delete|fn|pub|use|in|and|or|not|self|this)\b|\b(\d+(?:\.\d+)?)\b)re");
        auto matches=tokens.globalMatch(text);
        while(matches.hasNext()) {
            const auto match=matches.next();
            const auto color=fences?
                (match.capturedStart(1)>=0?(d?"#d69d85":"#a31515"):match.capturedStart(2)>=0?(d?"#569cd6":"#0000ff"):(d?"#b8d7a3":"#098658")):
                (match.capturedStart(1)>=0?(d?"#d6b48b":"#855000"):match.capturedStart(2)>=0?(d?"#8dbce9":"#245e9c"):(d?"#b6cfa0":"#466d23"));
            setFormat(match.capturedStart(),match.capturedLength(),QColor(color));
        }
        static const QRegularExpression comment(R"((?:^|\s)(#.*$|//.*$))");
        auto comments=comment.globalMatch(text);
        while(comments.hasNext()) {
            const auto match=comments.next();
            if(!(hash || !match.captured(1).startsWith('#'))) continue;
            if(fences && format(match.capturedStart(1)).foreground().color()==QColor(d?"#d69d85":"#a31515")) continue;
            QTextCharFormat format;
            format.setForeground(QColor(fences?(d?"#57a64a":"#008000"):(d?"#929b93":"#687368")));
            if(fences) format.setFontItalic(true);
            setFormat(match.capturedStart(1),match.capturedLength(1),format);
            break;
        }
        if (markdown) {
            if(text.startsWith('#')) { QTextCharFormat f; f.setForeground(QColor(d?"#8dbce9":"#245e9c")); f.setFontWeight(QFont::Bold); setFormat(0,text.size(),f); }
        }
    }
private:
    QWidget* owner;
    QString extension;
    bool hash=false,markdown=false,fences=false;
};
// Render Markdown locally. Remote URLs, HTML resource requests and paths outside
// the document's own directory never fetch content during a preview.
// Override the document loader itself: Qt's default document loader can fall
// back to filesystem access after a browser returns an invalid resource.
class MarkdownDocument final : public QTextDocument {
    struct ImageResource { QImage pixels; bool readable=false; };
public:
    using QTextDocument::QTextDocument;
    QString root;
    bool textOnly=false;
    int imageDecodes=0;
    void invalidateImages() { images.clear(); }
    int imageCacheCost() const { return images.totalCost(); }
    QImage imageForCopy(const QUrl& url) {
        const auto resolved=baseUrl().resolved(url);
        loadResource(QTextDocument::ImageResource,resolved);
        const auto* resource=images.object(resolved);
        return resource && resource->readable?resource->pixels:QImage{};
    }
protected:
    QVariant loadResource(int type,const QUrl& url) override {
        if(type!=QTextDocument::ImageResource) return {};
        if(const auto* cached=images.object(url)) return cached->pixels;
        // Overriding QTextDocument's loader bypasses its default cache. Layout
        // asks for each image repeatedly, even just to determine its dimensions.
        // Keep successful and failed requests within this document revision;
        // the cost includes entry overhead and is bounded independently of the
        // number of authored image references. No native loader fallback runs.
        const auto remember=[&](const QImage& image=QImage()) -> QVariant {
            // QTextImageHandler retries an invalid resource as a raw filename.
            // A small rendered placeholder keeps denied/missing inputs inside
            // this loader, including pipe paths that must never be opened.
            QImage value=image;
            if(value.isNull()) {
                value=QImage(16,16,QImage::Format_ARGB32_Premultiplied); value.fill(Qt::transparent);
                QPainter painter(&value); painter.setPen(QColor("#858585"));
                painter.drawRect(1,1,13,13); painter.drawLine(3,3,12,12); painter.drawLine(12,3,3,12);
            }
            const QVariant result=value;
            const auto cost=value.sizeInBytes()+1024+url.toEncoded().size();
            if(cost<=images.maxCost()) images.insert(url,new ImageResource{value,!image.isNull()},int(cost));
            return result;
        };
        if(textOnly || (!url.isRelative() && !url.isLocalFile())) return remember();
        const auto resolved=url.isRelative()?QDir(root).filePath(url.path()):url.toLocalFile();
        const auto canonical=QFileInfo(resolved).canonicalFilePath();
        const auto canonicalRoot=QFileInfo(root).canonicalFilePath();
        const auto boundary=canonicalRoot.endsWith(QDir::separator())?canonicalRoot:canonicalRoot+QDir::separator();
        const QFileInfo input(canonical);
        // QImageReader opens its input synchronously: reject pipes and devices
        // before asking it for dimensions, which could otherwise block the UI.
        if(canonicalRoot.isEmpty() || canonical.isEmpty() || !canonical.startsWith(boundary) || !input.isFile() || input.size()>8*1024*1024) return remember();
        QByteArray svg;
        QBuffer vectorInput;
        QFile resource(canonical);
        if(!resource.open(QIODevice::ReadOnly)) return remember();
        QImageReader reader(&resource);
        const auto format=reader.format();
        if(format.startsWith("svg")) {
            // Even size() parses SVG and can open nested images/stylesheets.
            // Validate a bounded snapshot before SVG sizing or rendering;
            // compressed SVG cannot be inspected within this loader's bounds.
            if(format!="svg" || !resource.seek(0)) return remember();
            svg=resource.read(8*1024*1024+1);
            if(svg.size()>8*1024*1024 || !selfContainedSvg(svg)) return remember();
            vectorInput.setData(svg); vectorInput.open(QIODevice::ReadOnly);
            reader.setDevice(&vectorInput); reader.setFormat("svg");
        }
        const auto size=reader.size();
        if(!size.isValid() || qint64(size.width())*size.height()>16*1024*1024) return remember();
        if(size.width()>1600 || size.height()>1600) reader.setScaledSize(size.scaled(1600,1600,Qt::KeepAspectRatio));
        ++imageDecodes;
        return remember(reader.read());
    }
private:
    static bool selfContainedSvg(const QByteArray& bytes) {
        QXmlStreamReader xml(bytes);
        int depth=0,elements=0;
        bool root=false;
        while(!xml.atEnd()) {
            const auto token=xml.readNext();
            if(token==QXmlStreamReader::ProcessingInstruction ||
                (token==QXmlStreamReader::DTD && !xml.entityDeclarations().isEmpty())) return false;
            if(token==QXmlStreamReader::StartElement) {
                if(!root) { if(xml.name()!=QLatin1String("svg")) return false; root=true; }
                if(++depth>128 || ++elements>100000) return false;
                // Nested rasters, including data URLs, bypass the image pixel
                // limit. Vector definitions and internal use/gradient refs work.
                if(xml.name()==QLatin1String("image") || xml.name()==QLatin1String("feImage")) return false;
                for(const auto& attribute:xml.attributes())
                    if(attribute.name()==QLatin1String("href") && !attribute.value().trimmed().startsWith('#')) return false;
            } else if(token==QXmlStreamReader::EndElement) --depth;
        }
        return root && !xml.hasError();
    }
    QCache<QUrl,ImageResource> images{64*1024*1024};
};
// A native edit block groups notifications but does not defer font, frame or
// viewport-width layout. Keep nested preparation/resize changes in one pass.
class DocumentLayoutBatch final {
public:
    explicit DocumentLayoutBatch(QTextDocument* document):document(document),enabled(document->isLayoutEnabled()) { document->setLayoutEnabled(false); }
    ~DocumentLayoutBatch() { document->setLayoutEnabled(enabled); }
    DocumentLayoutBatch(const DocumentLayoutBatch&)=delete;
    DocumentLayoutBatch& operator=(const DocumentLayoutBatch&)=delete;
private:
    QTextDocument* document;
    bool enabled;
};
class MarkdownBrowser final : public QTextBrowser {
public:
    explicit MarkdownBrowser(QWidget* parent=nullptr):QTextBrowser(parent) {
        // Qt can round a fitted table one pixel past an odd viewport width.
        // Reserve that rounding allowance in layout instead of concealing a
        // scrollbar that may be needed for genuinely oversized content.
        setLineWrapMode(QTextEdit::FixedPixelWidth);
        setLineWrapColumnOrWidth(std::max(1,viewport()->width()-1));
        for(const auto& key:{QString("Ctrl++"),QString("Ctrl+="),QString("Ctrl+Shift++"),QString("Ctrl+-")}) {
            auto* shortcut=new QShortcut(QKeySequence(key),this);
            shortcut->setContext(Qt::WidgetWithChildrenShortcut);
            connect(shortcut,&QShortcut::activated,this,[this,key] {
                changeZoom(zoomPercent+(key=="Ctrl+-"?-10:10));
            });
        }
        setToolTip("Zoom: Ctrl+scroll or Ctrl++ / Ctrl+-");
    }
    int zoom() const { return zoomPercent; }
    void prepareContent() {
        // Qt can leave an extra empty paragraph in the first header cell after
        // a fenced block. Remove that parser artifact, preserving empty cells.
        std::function<void(QTextFrame*)> cleanTables=[&](QTextFrame* frame) {
            for(auto* child:frame->childFrames()) {
                if(auto* table=dynamic_cast<QTextTable*>(child))
                    for(int row=0;row<table->rows();++row) for(int column=0;column<table->columns();++column) {
                        auto cell=table->cellAt(row,column); auto first=cell.firstCursorPosition();
                        while(first.block().text().isEmpty() && first.block()!=cell.lastCursorPosition().block()) first.deleteChar();
                    }
                cleanTables(child);
            }
        };
        cleanTables(document()->rootFrame());
        const auto wrapRange=[&](int first,int last,int kind) {
            const auto firstFormat=document()->findBlock(first).blockFormat();
            QTextCursor cursor(document()); cursor.setPosition(first); cursor.setPosition(last,QTextCursor::KeepAnchor);
            QTextFrameFormat format; format.setProperty(kind,true);
            auto* frame=cursor.insertFrame(format);
            // insertFrame replaces its first paragraph's block format. Retain
            // quote depth and fence language instead of losing the first line.
            auto firstCursor=frame->firstCursorPosition(); firstCursor.setBlockFormat(firstFormat);
            for(const auto& block:{frame->firstCursorPosition().block().previous(),frame->lastCursorPosition().block().next()}) {
                if(block.isValid() && block.text().isEmpty()) {
                    QTextCursor boundary(block); auto value=block.blockFormat();
                    value.setProperty(FrameBoundary,true);
                    value.clearProperty(QTextFormat::BlockQuoteLevel);
                    value.clearProperty(QTextFormat::BlockCodeFence);
                    value.clearProperty(QTextFormat::BlockCodeLanguage);
                    value.setNonBreakableLines(false);
                    boundary.setBlockFormat(value);
                }
            }
        };
        // Quote frames supply one continuous shaded surface. Nested quote
        // levels retain their semantic indentation and their own left bars.
        QVector<QPair<int,int>> quotes;
        for(auto block=document()->begin();block.isValid();block=block.next()) {
            if(block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel)==0) continue;
            const int first=block.position();
            while(block.next().isValid() && block.next().blockFormat().intProperty(QTextFormat::BlockQuoteLevel)>0) block=block.next();
            quotes.append({first,block.position()+block.length()-1});
        }
        for(auto it=quotes.crbegin();it!=quotes.crend();++it) wrapRange(it->first,it->second,QuoteFrame);
        QVector<QPair<int,int>> code;
        for(auto block=document()->begin();block.isValid();block=block.next()) {
            if(!markdownCode(block)) continue;
            const int first=block.position();
            while(markdownCode(block.next())) block=block.next();
            code.append({first,block.position()+block.length()-1});
        }
        for(auto it=code.crbegin();it!=code.crend();++it) wrapRange(it->first,it->second,CodeFrame);
        // A trailing empty block supplies reading space without changing the
        // root frame on resize, which would invalidate every paragraph.
        auto end=document()->rootFrame()->lastCursorPosition();
        if(!end.block().text().isEmpty()) end.insertBlock(QTextBlockFormat(),QTextCharFormat());
        QTextBlockFormat space; space.setProperty(ReadingSpace,true);
        end.setBlockFormat(space); readingSpace=end.block();
        // Dimensions must outlive pixel-cache eviction: asking for every image
        // on each resize would thrash the LRU for screenshot-heavy documents.
        // Entries are bounded by image characters in the admitted source text.
        imagePositions.clear();
        for(auto block=document()->begin();block.isValid();block=block.next())
            for(auto it=block.begin();!it.atEnd();++it) {
                const auto fragment=it.fragment();
                // Identical adjacent images can share one text fragment.
                if(fragment.charFormat().isImageFormat())
                    for(int i=0;i<fragment.length();++i) {
                        const auto image=fragment.charFormat().toImageFormat();
                        imagePositions.append(ImagePosition{fragment.position()+i,{},QSizeF(image.width(),image.height())});
                    }
            }
    }
    void presentation() {
        // setMarkdown() builds formats directly and bypasses the document CSS.
        // Style its semantic blocks in place so search, selection and safe image
        // loading retain the same native document across palette changes.
        const int vertical=verticalScrollBar()->value(),horizontal=horizontalScrollBar()->value();
        const DocumentLayoutBatch batch(document());
        const double scale=zoomPercent/100.;
        auto bodyFont=font(); bodyFont.setPixelSize(qRound(14*scale));
        document()->setDefaultFont(bodyFont);
        // Code's preview applies a 26 px inset to both html and body.
        document()->setDocumentMargin(52*scale);
        document()->setIndentWidth(40*scale);
        auto frame=document()->rootFrame()->frameFormat(); frame.setTopMargin(14*scale);
        frame.setBottomMargin(0);
        document()->rootFrame()->setFrameFormat(frame);
        const bool d=dark(this);
        const QColor pageColor(d?"#1f1f1f":"#ffffff"),textColor(d?"#cccccc":"#3b3b3b");
        const QColor codeColor(d?"#2b2b2b":"#f8f8f8");
        const QColor inlineColor(d?"#3c3c3c":"#e0e0e0");
        auto surface=viewport()->palette(); surface.setColor(QPalette::Base,pageColor); surface.setColor(QPalette::Window,pageColor);
        viewport()->setPalette(surface);
        frame.setBackground(pageColor); document()->rootFrame()->setFrameFormat(frame);
        const auto rule=palette().color(QPalette::Text);
        QColor border=rule; border.setAlphaF(.18);
        QTextCursor cursor(document()); cursor.beginEditBlock();
        for(auto block=document()->begin();block.isValid();block=block.next()) {
            cursor.setPosition(block.position());
            auto format=block.blockFormat();
            const int heading=format.headingLevel(),quote=format.intProperty(QTextFormat::BlockQuoteLevel);
            const bool code=markdownCode(block),table=cursor.currentTable()!=nullptr;
            static constexpr double headingScale[]={2,1.5,1.25,1,.875,.85};
            format.setTopMargin(heading && block!=document()->begin()?24*scale:0);
            const double size=14*scale*headingScale[std::clamp(heading,1,6)-1];
            const double headingPadding=heading>0 && heading<=2?size*.3+scale:0;
            format.setBottomMargin(table || code?0:heading?16*scale+headingPadding:block.textList()?0:16*scale);
            bool imageBlock=false;
            for(auto it=block.begin();!it.atEnd();++it) imageBlock|=it.fragment().charFormat().isImageFormat();
            format.setLineHeight(heading?size*1.25:(code?19:22.4)*scale,imageBlock?QTextBlockFormat::MinimumHeight:QTextBlockFormat::FixedHeight);
            if(code) {
                format.setLeftMargin(0); format.setRightMargin(0);
                format.setTopMargin(0);
                // Match the final baseline leading in Code's pre/code layout.
                format.setBottomMargin(markdownCode(block.next())?0:2.4*scale);
                // Long code stays readable on narrow cards instead of forcing
                // the entire document to scroll sideways.
                format.setNonBreakableLines(false);
            } else if(quote) {
                format.setLeftMargin(quote*15*scale); format.setRightMargin(quote*16*scale);
                auto next=block.next();
                // Native panel boundaries have no quote metadata. Look beyond
                // them so an embedded fence does not erase paragraph spacing.
                while(next.isValid() && next.blockFormat().boolProperty(FrameBoundary)) next=next.next();
                if(next.blockFormat().intProperty(QTextFormat::BlockQuoteLevel)==0) format.setBottomMargin(0);
            }
            if(auto* list=block.textList()) {
                auto value=list->format();
                if(value.style()==QTextListFormat::ListDisc || value.style()==QTextListFormat::ListCircle || value.style()==QTextListFormat::ListSquare) {
                    value.setStyle(value.indent()==1?QTextListFormat::ListDisc:value.indent()==2?QTextListFormat::ListCircle:QTextListFormat::ListSquare);
                    if(value!=list->format()) list->setFormat(value);
                }
                const auto* next=block.next().textList();
                if(!next || (next!=list && next->format().indent()==value.indent())) format.setBottomMargin(9.8*scale);
            }
            if(format.boolProperty(FrameBoundary) || (block.text().isEmpty() && cursor.currentFrame()==document()->rootFrame() && (block.previous().isValid() || block.next().isValid()))) {
                QTextCursor previous(block.previous());
                format.setTopMargin(0); format.setBottomMargin(previous.currentTable()?9.8*scale:0);
                format.setLineHeight(.01,QTextBlockFormat::FixedHeight);
            }
            if(block==readingSpace) {
                format=QTextBlockFormat(); format.setProperty(ReadingSpace,true);
                format.setLineHeight(std::max(0.,viewport()->height()-22.4*scale),QTextBlockFormat::FixedHeight);
            } else if(block.next()==readingSpace && cursor.currentFrame()==document()->rootFrame()) {
                // Qt ignores the final paragraph margin at the root boundary.
                format.setBottomMargin(0);
            }
            cursor.setBlockFormat(format);
            if(block.length()>1) {
                cursor.setPosition(block.position()); cursor.setPosition(block.position()+block.length()-1,QTextCursor::KeepAnchor);
                QTextCharFormat body; body.setForeground(textColor); cursor.mergeCharFormat(body);
            }
            if(heading) {
                for(auto it=block.begin();!it.atEnd();++it) {
                    const auto fragment=it.fragment(); auto text=fragment.charFormat();
                    text.clearProperty(QTextFormat::FontSizeAdjustment);
                    text.clearProperty(QTextFormat::FontPointSize);
                    text.setProperty(QTextFormat::FontPixelSize,qRound(size));
                    // Noto's native 600 face resolves to Medium on this host;
                    // its Bold face matches Chromium's semibold fallback.
                    text.setFontWeight(QFont::Bold);
                    cursor.setPosition(fragment.position()); cursor.setPosition(fragment.position()+fragment.length(),QTextCursor::KeepAnchor);
                    cursor.setCharFormat(text);
                }
            }
            for(auto it=block.begin();!it.atEnd();++it) {
                const auto fragment=it.fragment();
                if(!fragment.charFormat().fontFixedPitch()) continue;
                QTextCharFormat text; text.setBackground(inlineColor); text.setForeground(d?QColor("#d0d0d0"):textColor);
                cursor.setPosition(fragment.position()); cursor.setPosition(fragment.position()+fragment.length(),QTextCursor::KeepAnchor);
                cursor.mergeCharFormat(text);
            }
        }
        std::function<void(QTextFrame*)> tables=[&](QTextFrame* frame) {
            for(auto* child:frame->childFrames()) {
                auto next=child->lastCursorPosition().block().next();
                while(next.isValid() && next.blockFormat().boolProperty(FrameBoundary)) next=next.next();
                const bool headingNext=next.blockFormat().headingLevel()>0;
                if(child->frameFormat().boolProperty(CodeFrame)) {
                    auto format=child->frameFormat(); format.setPadding(16*scale);
                    const int quote=child->firstCursorPosition().blockFormat().intProperty(QTextFormat::BlockQuoteLevel);
                    format.setLeftMargin(quote*15*scale); format.setRightMargin(quote*16*scale);
                    format.setBackground(codeColor); format.setBorder(1); format.setBorderBrush(QColor(d?"#313131":"#e5e5e5"));
                    format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid); format.setBottomMargin(headingNext?0:14*scale);
                    child->setFrameFormat(format);
                }
                if(child->frameFormat().boolProperty(QuoteFrame)) {
                    auto format=child->frameFormat(); format.setPadding(0);
                    // CSS collapses the following heading's larger top margin
                    // with the panel gap; native frame margins otherwise add.
                    format.setBackground(codeColor); format.setBorder(0); format.setBottomMargin(headingNext?0:16*scale);
                    child->setFrameFormat(format);
                }
                if(auto* table=dynamic_cast<QTextTable*>(child)) {
                    auto format=table->format(); format.setBorder(0); format.setCellSpacing(0); format.setCellPadding(0);
                    // Qt omits the preceding paragraph's bottom margin at a
                    // table boundary, including H1/H2 rule padding.
                    const auto before=table->firstCursorPosition().block().previous();
                    format.setTopMargin(before.blockFormat().bottomMargin());
                    format.setBottomMargin(0); table->setFormat(format);
                    for(int row=0;row<table->rows();++row) for(int column=0;column<table->columns();++column) {
                        auto cell=table->cellAt(row,column); auto cellFormat=cell.format().toTableCellFormat();
                        cellFormat.setVerticalAlignment(QTextCharFormat::AlignTop);
                        cellFormat.setLeftPadding(10*scale); cellFormat.setRightPadding(10*scale);
                        cellFormat.setTopPadding(5*scale); cellFormat.setBottomPadding(5*scale);
                        cellFormat.setBorder(0);
                        cellFormat.setBottomBorder(row<table->rows()-1?1:0);
                        cellFormat.setBottomBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                        auto color=border; if(row==0) color.setAlphaF(.69);
                        cellFormat.setBottomBorderBrush(color); cell.setFormat(cellFormat);
                        if(row==0) {
                            auto header=cell.firstCursorPosition(); header.setPosition(cell.lastCursorPosition().position(),QTextCursor::KeepAnchor);
                            QTextCharFormat bold; bold.setFontWeight(QFont::Bold); header.mergeCharFormat(bold);
                        }
                    }
                }
                tables(child);
            }
        };
        tables(document()->rootFrame());
        cursor.endEditBlock();
        colors();
        if(!syntax) syntax=new Syntax(document(),this,true);
        syntax->rehighlight();
        fitImages();
        verticalScrollBar()->setValue(vertical); horizontalScrollBar()->setValue(horizontal);
        viewport()->update();
    }
    void refreshImages() {
        for(auto& image:imagePositions) image.size={};
        fitImages();
        document()->markContentsDirty(0,document()->characterCount());
    }
    void colors() {
        const auto color=palette().color(QPalette::Link);
        QVector<QPair<int,int>> links;
        for(auto block=document()->begin();block.isValid();block=block.next())
            for(auto it=block.begin();!it.atEnd();++it) {
                const auto fragment=it.fragment();
                const auto format=fragment.charFormat();
                if(!format.anchorHref().isEmpty() && format.foreground().color()!=color)
                    links.append({fragment.position(),fragment.length()});
            }
        // Anchor colors are stored in the document. Updating just their color
        // preserves selection, formatting, images and the cached Markdown text.
        QTextCursor cursor(document()); cursor.beginEditBlock();
        QTextCharFormat format; format.setForeground(color);
        for(const auto& link:links) {
            cursor.setPosition(link.first); cursor.setPosition(link.first+link.second,QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(format);
        }
        cursor.endEditBlock();
    }
protected:
    void wheelEvent(QWheelEvent* event) override {
        if(event->modifiers()==Qt::ControlModifier) {
            wheelZoom+=event->angleDelta().y()?event->angleDelta().y()/120.:event->pixelDelta().y()/40.;
            const int steps=int(wheelZoom); wheelZoom-=steps;
            if(steps) changeZoom(zoomPercent+10*steps);
            event->accept(); return;
        }
        wheelZoom=0;
        QTextBrowser::wheelEvent(event);
    }
    QMimeData* createMimeDataFromSelection() const override {
        const auto pixels=copyPixels(textCursor());
        return pixels.isNull()?QTextBrowser::createMimeDataFromSelection():imageMimeData(pixels);
    }
    void mousePressEvent(QMouseEvent* event) override {
        const auto image=imageAt(event->position().toPoint());
        QTextBrowser::mousePressEvent(event);
        if(event->button()==Qt::LeftButton && event->modifiers()==Qt::NoModifier && !image.isNull())setTextCursor(image);
    }
    void contextMenuEvent(QContextMenuEvent* event) override {
        const auto image=event->reason()==QContextMenuEvent::Keyboard?textCursor():imageAt(event->pos());
        std::unique_ptr<QMenu> menu(createStandardContextMenu(event->pos()));
        if(!image.isNull() && image.charFormat().isImageFormat()) {
            const auto pixels=copyPixels(image);
            auto* action=new QAction("Copy Image",menu.get());
            action->setObjectName("copyMarkdownImage"); action->setEnabled(!pixels.isNull());
            const auto first=menu->actions().value(0);
            menu->insertAction(first,action); menu->insertSeparator(first);
            connect(action,&QAction::triggered,this,[pixels] { QApplication::clipboard()->setMimeData(imageMimeData(pixels)); });
        }
        menu->exec(event->globalPos());
    }
    void resizeEvent(QResizeEvent* event) override {
        QTextBrowser::resizeEvent(event);
        const int wrapWidth=std::max(1,viewport()->width()-1);
        if(lineWrapColumnOrWidth()!=wrapWidth)setLineWrapColumnOrWidth(wrapWidth);
        if(readingSpace.isValid() && readingSpace.blockFormat().boolProperty(ReadingSpace)) {
            auto format=readingSpace.blockFormat();
            const double height=std::max(0.,viewport()->height()-22.4*zoomPercent/100.);
            if(!qFuzzyCompare(format.lineHeight()+1,height+1)) {
                format.setLineHeight(height,QTextBlockFormat::FixedHeight);
                QTextCursor(readingSpace).setBlockFormat(format);
            }
        }
        fitImages();
    }
    void paintEvent(QPaintEvent* event) override {
        QTextBrowser::paintEvent(event);
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        auto rule=palette().color(QPalette::Text); rule.setAlphaF(.18);
        painter.setPen(rule);
        const QPointF offset(-horizontalScrollBar()->value(),-verticalScrollBar()->value());
        const double scale=zoomPercent/100.;
        QSet<QTextFrame*> decorated;
        for(auto block=cursorForPosition(QPoint(0,0)).block();block.isValid();block=block.next()) {
            const auto rect=document()->documentLayout()->blockBoundingRect(block).translated(offset);
            if(rect.bottom()<0) continue;
            if(rect.top()>viewport()->height()) break;
            const auto format=block.blockFormat();
            if(format.headingLevel()==1 || format.headingLevel()==2) {
                const double size=(format.headingLevel()==1?28:21)*scale;
                const double y=rect.top()+size*1.25+size*.3;
                painter.setPen(rule);
                painter.drawLine(QPointF(rect.left(),y),QPointF(rect.right(),y));
            }
            QTextCursor cursor(block);
            for(auto* frame=cursor.currentFrame();frame && frame!=document()->rootFrame();frame=frame->parentFrame()) {
                if(decorated.contains(frame)) continue;
                decorated.insert(frame);
                const auto style=frame->frameFormat();
                const auto bounds=document()->documentLayout()->frameBoundingRect(frame).translated(offset)
                    .adjusted(style.leftMargin(),style.topMargin(),-style.rightMargin(),-style.bottomMargin());
                if(style.boolProperty(QuoteFrame)) painter.fillRect(QRectF(bounds.left(),bounds.top(),5*scale,bounds.height()),QColor(dark(this)?"#616161":"#e5e5e5"));
                if(style.boolProperty(CodeFrame)) {
                    QPainterPath square,rounded;
                    square.addRect(bounds); rounded.addRoundedRect(bounds,3,3);
                    const auto parent=frame->parentFrame()->frameFormat().background();
                    painter.fillPath(square.subtracted(rounded),parent.style()==Qt::NoBrush?palette().brush(QPalette::Base):parent);
                    painter.setPen(style.borderBrush().color()); painter.setBrush(Qt::NoBrush);
                    painter.drawRoundedRect(bounds.adjusted(.5,.5,-.5,-.5),3,3);
                }
            }
            const int quote=format.intProperty(QTextFormat::BlockQuoteLevel);
            if(quote>1) {
                for(int level=1;level<quote;++level)
                    painter.fillRect(QRectF((52+level*15)*scale+offset.x(),rect.top(),5*scale,rect.height()+format.bottomMargin()),QColor(dark(this)?"#616161":"#e5e5e5"));
            }
        }
    }
    void changeEvent(QEvent* event) override {
        QTextBrowser::changeEvent(event);
        if(event->type()==QEvent::PaletteChange || event->type()==QEvent::FontChange) presentation();
    }
private:
    int zoomPercent=100;
    double wheelZoom=0;
    void changeZoom(int percent) {
        percent=std::clamp(percent,50,300);
        if(percent==zoomPercent) return;
        // Anchor the first visible text through reflow instead of preserving a
        // pixel scroll offset that would jump to a different paragraph.
        const auto anchor=cursorForPosition(QPoint(viewport()->width()/2,0));
        const int top=cursorRect(anchor).top();
        zoomPercent=percent;
        presentation();
        verticalScrollBar()->setValue(verticalScrollBar()->value()+cursorRect(anchor).top()-top);
    }
    static QMimeData* imageMimeData(const QImage& image) {
        auto* mime=new QMimeData; mime->setImageData(image);
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
        if(image.save(&buffer,"PNG"))mime->setData("image/png",png);
        return mime;
    }
    QImage copyPixels(const QTextCursor& cursor) const {
        if(!cursor.hasSelection() || cursor.selectionEnd()-cursor.selectionStart()!=1 || !cursor.charFormat().isImageFormat())return {};
        return static_cast<MarkdownDocument*>(document())->imageForCopy(QUrl(cursor.charFormat().toImageFormat().name()));
    }
    QTextCursor imageAt(QPoint point) const {
        const auto offset=QPoint(horizontalScrollBar()->value(),verticalScrollBar()->value());
        const int hit=document()->documentLayout()->hitTest(point+offset,Qt::ExactHit);
        if(hit<0)return {};
        // Inline-object hit tests choose either edge depending on which half
        // was clicked. Check both positions without treating nearby text as an image.
        for(int position:{hit,hit-1}) {
            if(position<0 || position>=document()->characterCount()-1)continue;
            QTextCursor cursor(document()); cursor.setPosition(position);
            const auto left=cursorRect(cursor).left();
            cursor.movePosition(QTextCursor::NextCharacter,QTextCursor::KeepAnchor);
            if(cursor.charFormat().isImageFormat() && point.x()>=left && point.x()<left+cursor.charFormat().toImageFormat().width())return cursor;
        }
        return {};
    }
    enum { CodeFrame=QTextFormat::UserProperty+1,FrameBoundary,QuoteFrame,ReadingSpace };
    Syntax* syntax=nullptr;
    struct ImagePosition { int position; QSize size; QSizeF authored; };
    QVector<ImagePosition> imagePositions;
    QTextBlock readingSpace;
    bool fittingImages=false;
    void fitImages() {
        if(fittingImages || imagePositions.isEmpty()) return;
        fittingImages=true;
        QTextCursor cursor(document()); cursor.beginEditBlock();
        for(auto& entry:imagePositions) {
            const int position=entry.position;
            if(position<0 || position>=document()->characterCount()-1) continue;
            cursor.setPosition(position); cursor.movePosition(QTextCursor::NextCharacter,QTextCursor::KeepAnchor);
            if(!cursor.charFormat().isImageFormat()) continue;
            auto format=cursor.charFormat().toImageFormat();
            if(!entry.size.isValid())
                entry.size=document()->resource(QTextDocument::ImageResource,QUrl(format.name())).value<QImage>().size();
            if(!entry.size.isValid()) continue;
            const auto block=cursor.block();
            const auto blockFormat=block.blockFormat();
            double available=std::max(1,viewport()->width()-1)-2*document()->documentMargin()-blockFormat.leftMargin()-blockFormat.rightMargin();
            if(block.textList()) available-=block.textList()->format().indent()*document()->indentWidth();
            for(auto* frame=cursor.currentFrame();frame && frame!=document()->rootFrame();frame=frame->parentFrame()) {
                const auto value=frame->frameFormat();
                available-=value.leftMargin()+value.rightMargin()+2*(value.padding()+value.border());
                if(auto* table=dynamic_cast<QTextTable*>(frame)) {
                    const auto cell=table->cellAt(cursor); const auto cellStyle=cell.format().toTableCellFormat();
                    available=available*cell.columnSpan()/std::max(1,table->columns())-cellStyle.leftPadding()-cellStyle.rightPadding();
                }
            }
            QSizeF desired=entry.size;
            if(entry.authored.width()>0) {
                desired.setWidth(entry.authored.width());
                desired.setHeight(entry.authored.height()>0?entry.authored.height():desired.width()*entry.size.height()/entry.size.width());
            } else if(entry.authored.height()>0) {
                desired.setHeight(entry.authored.height()); desired.setWidth(desired.height()*entry.size.width()/entry.size.height());
            }
            desired*=zoomPercent/100.;
            // Fractional table columns can round each image's minimum width
            // upward in Qt's layout and create a spurious one-pixel scrollbar.
            const double width=std::min(desired.width(),std::floor(std::max(1.,available)));
            const double height=width*desired.height()/desired.width();
            if(!qFuzzyCompare(format.width()+1,width+1) || !qFuzzyCompare(format.height()+1,height+1)) {
                format.setWidth(width); format.setHeight(height); cursor.setCharFormat(format);
            }
        }
        cursor.endEditBlock(); fittingImages=false;
    }
};
}

class CodePane final : public QTextEdit {
public:
    explicit CodePane(QWidget* parent=nullptr):QTextEdit(parent),syntax(new Syntax(document(),this)) {
        setReadOnly(true); setFrameShape(QFrame::NoFrame);
        setAcceptRichText(false);
        setUndoRedoEnabled(false);
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        auto f=font(); f.setPointSizeF(std::max(10.,f.pointSizeF())); setFont(f);
        setTabStopDistance(fontMetrics().horizontalAdvance(' ')*4);
        setLineWrapMode(QTextEdit::NoWrap);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        gutter=new QWidget(this); gutter->installEventFilter(this);
        connect(verticalScrollBar(),&QScrollBar::valueChanged,gutter,qOverload<>(&QWidget::update));
        connect(document()->documentLayout(),&QAbstractTextDocumentLayout::update,this,[this](const QRectF&) { gutter->update(); });
        setAccessibleName("File source");
    }
    QVector<FilePreview::Row> rows;
    QVector<QPair<int,int>> emphasis;
    bool dual=false;
    Syntax* syntax;
    std::function<void()> resized;
    void content(const QVector<FilePreview::Row>& value,const QString& extension,bool both=false) {
        rows=value; dual=both; const bool languageChanged=syntax->language(extension);
        QStringList text; text.reserve(rows.size());
        for(const auto& row:rows) text.append(row.text);
        const auto next=text.join('\n');
        const bool textChanged=next!=toPlainText();
        if(textChanged) {
            const int position=textCursor().position(),vertical=verticalScrollBar()->value(),horizontal=horizontalScrollBar()->value();
            setPlainText(next);
            auto cursor=textCursor(); cursor.setPosition(std::min(position,document()->characterCount()-1)); setTextCursor(cursor);
            verticalScrollBar()->setValue(vertical); horizontalScrollBar()->setValue(horizontal);
        }
        // setPlainText already schedules syntax highlighting for new text.
        // Recoloring row backgrounds must not highlight that text twice.
        margin(); colors(!textChanged && languageChanged);
    }
    void colors(bool highlight=true) {
        if(highlight) syntax->rehighlight();
        QList<QTextEdit::ExtraSelection> highlights;
        auto block=document()->firstBlock();
        for(int i=0;i<rows.size() && block.isValid();++i,block=block.next()) {
            const auto kind=rows[i].kind;
            if(kind==' ' || kind.isNull()) continue;
            QTextEdit::ExtraSelection selection;
            selection.cursor=QTextCursor(block); selection.format.setBackground(tint(this,kind));
            selection.format.setProperty(QTextFormat::FullWidthSelection,true);
            highlights.append(selection);
            if(i<emphasis.size() && emphasis[i].second>0) {
                selection.cursor.setPosition(block.position()+emphasis[i].first);
                selection.cursor.setPosition(block.position()+emphasis[i].first+emphasis[i].second,QTextCursor::KeepAnchor);
                selection.format.clearProperty(QTextFormat::FullWidthSelection);
                selection.format.setBackground(tint(this,kind,true)); highlights.append(selection);
            }
        }
        setExtraSelections(highlights); gutter->update();
    }
protected:
    void resizeEvent(QResizeEvent* e) override { QTextEdit::resizeEvent(e); gutter->setGeometry(0,0,marginWidth(),height()); if(resized) resized(); }
    void changeEvent(QEvent* event) override {
        QTextEdit::changeEvent(event);
        // Parent palette events can arrive before a pane inherits its colors.
        if(event->type()==QEvent::PaletteChange && gutter) colors();
        if(event->type()==QEvent::FontChange && gutter) {
            setTabStopDistance(fontMetrics().horizontalAdvance(' ')*4); margin();
            if(resized) resized();
        }
    }
    bool eventFilter(QObject* object,QEvent* event) override {
        if(object==gutter && event->type()==QEvent::Paint) {
            QPainter p(gutter); p.fillRect(gutter->rect(),palette().color(QPalette::AlternateBase));
            const auto* layout=document()->documentLayout();
            const int first=layout->hitTest(QPointF(0,verticalScrollBar()->value()),Qt::FuzzyHit);
            auto block=document()->findBlock(std::max(0,first));
            while(block.isValid()) {
                const auto bounds=layout->blockBoundingRect(block).translated(0,-verticalScrollBar()->value());
                if(bounds.top()>height()) break;
                const int index=block.blockNumber();
                if(index<rows.size() && block.isVisible()) {
                    const auto& row=rows[index];
                    if(row.kind=='+' || row.kind=='-') p.fillRect(QRectF(0,bounds.top(),gutter->width(),bounds.height()),tint(this,row.kind));
                    p.setPen(palette().color(QPalette::PlaceholderText));
                    QString number=row.before?QString::number(row.before):QString();
                    if(dual) number=number.rightJustified(5,' ')+" "+(row.after?QString::number(row.after):QString()).rightJustified(5,' ');
                    else if(row.after) number=QString::number(row.after);
                    const QString sign=(row.kind=='+' || row.kind=='-')?QString(row.kind):QString(" ");
                    p.drawText(QRectF(0,bounds.top(),gutter->width()-8,fontMetrics().height()),Qt::AlignRight|Qt::AlignVCenter,number+" "+sign);
                }
                block=block.next();
            }
            return true;
        }
        return QTextEdit::eventFilter(object,event);
    }
private:
    QWidget* gutter=nullptr;
    int marginWidth() const { return fontMetrics().horizontalAdvance('0')*(dual?14:8)+12; }
    void margin() { setViewportMargins(marginWidth(),0,0,0); gutter->setGeometry(0,0,marginWidth(),height()); gutter->update(); }
};

QImage FilePreview::thumbnail(const QJsonObject& config,QString& error) {
    const auto path=config["path"].toString(); QString text;
    if(!readText(path,text,error))return {};
    // A first-page derivative, not a hidden ordinary FileView. Bound parsing
    // independently of the full-document limits and never fetch subresources.
    text=text.left(16*1024).section('\n',0,95);
    const auto extension=QFileInfo(path).suffix().toLower();
    const bool markdown=QStringList{"md","markdown","mdown"}.contains(extension) &&
        config["settings"].toObject()["file_mode"]!="source";
    std::unique_ptr<QTextEdit> pane;
    if(markdown) {
        auto* browser=new MarkdownBrowser;
        auto* document=new MarkdownDocument(browser); document->textOnly=true;
        browser->setDocument(document); pane.reset(browser);
        browser->setFrameShape(QFrame::NoFrame); browser->resize(640,400); browser->ensurePolished();
        auto* syntax=new Syntax(document,browser,true);
        FilePreview::setMarkdown(document,text);
        browser->prepareContent(); browser->presentation(); syntax->rehighlight();
    } else {
        auto* source=new CodePane; pane.reset(source);
        source->resize(640,400); source->ensurePolished();
        QVector<Row> rows; const auto lines=text.split('\n');
        for(int i=0;i<lines.size();++i)rows.append({' ',lines[i],0,i+1});
        source->content(rows,extension); source->syntax->rehighlight();
    }
    pane->setReadOnly(true); pane->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pane->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pane->verticalScrollBar()->setValue(0);pane->horizontalScrollBar()->setValue(0);
    return pane->grab().toImage().scaled(160,100,Qt::KeepAspectRatio,Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888);
}

FileView::FileView(QWidget* parent):QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus); setObjectName("filePreview");
    setMinimumSize(100,100);
    auto* layout=new QVBoxLayout(this);
    // Allow the requested narrow size to arrive before reflowing the toolbar.
    // Otherwise the wide row's minimum hint prevents its own responsive branch.
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(0,0,0,0); layout->setSpacing(0);
    auto* controls=new QWidget(this); auto* bar=new QHBoxLayout(controls);
    toolbar=bar;
    controls->setObjectName("fileToolbar");
    bar->setContentsMargins(6,3,6,3); bar->setSpacing(6);
    mode=new QComboBox(controls); mode->setObjectName("fileMode"); mode->setAccessibleName("File view mode");
    mode->setMinimumContentsLength(12); mode->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    wrap=new QCheckBox("Soft wrap",controls); wrap->setObjectName("fileWrap");
    wrap->setToolTip("Soft wrap long lines (Alt+Z)"); wrap->setAccessibleName("Soft wrap (Alt+Z)");
    context=new QCheckBox("Full context",controls); context->setObjectName("fileContext");
    bar->addWidget(mode); bar->addWidget(wrap); bar->addWidget(context);
    auto button=[&](QString text,QString tip,auto callback) {
        auto* b=new QToolButton(controls); b->setText(text); b->setToolTip(tip); b->setAccessibleName(tip); b->setMinimumSize(28,28);
        connect(b,&QToolButton::clicked,this,callback); bar->addWidget(b); return b;
    };
    summary=new QLabel(controls); summary->setObjectName("fileSummary"); summary->setTextFormat(Qt::RichText);
    summary->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse); bar->addWidget(summary,1);
    searchTools=new QWidget(controls); searchToolbar=new QHBoxLayout(searchTools);
    searchTools->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);
    searchToolbar->setContentsMargins(0,0,0,0); searchToolbar->setSpacing(6);
    search=new QLineEdit(controls); search->setObjectName("fileSearch"); search->setPlaceholderText("Find in file…"); search->setAccessibleName("Find in file"); search->setMaximumWidth(220); search->setMinimumWidth(80);
    searchToolbar->addWidget(search);
    searchStatus=new QLabel(controls); searchStatus->setTextFormat(Qt::PlainText); searchToolbar->addWidget(searchStatus);
    auto* nextMatch=button("↓","Next match",[this] { find(); });
    searchToolbar->addWidget(nextMatch); bar->addWidget(searchTools);
    layout->addWidget(controls);
    filePicker=new ChangePicker(this); filePicker->setItemDelegate(new ChangeDelegate(filePicker)); filePicker->setObjectName("changedFilePicker"); filePicker->setAccessibleName("Changed file");
    filePicker->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed); layout->addWidget(filePicker);
    body=new QSplitter(this);
    files=new QTreeWidget(body); files->setObjectName("changedFiles"); files->setHeaderLabel("Changed files"); files->setColumnCount(1); files->setMinimumWidth(120); files->setMaximumWidth(420); files->setRootIsDecorated(false);
    files->setItemDelegate(new ChangeDelegate(files));
    pages=new QStackedWidget(body);
    source=new CodePane(pages); source->setObjectName("fileSource"); pages->addWidget(source);
    rendered=new MarkdownBrowser(pages); rendered->setDocument(new MarkdownDocument(rendered));
    rendered->setObjectName("fileMarkdown"); rendered->setFrameShape(QFrame::NoFrame); rendered->setOpenLinks(false); rendered->setOpenExternalLinks(false);
    // Keep image/text actions reachable through the window's generic card menu filter.
    rendered->setProperty("previewContentContextMenu",true); rendered->viewport()->setProperty("previewContentContextMenu",true);
    rendered->document()->setDocumentMargin(20); pages->addWidget(rendered);
    inlineDiff=new CodePane(pages); inlineDiff->setObjectName("fileInline"); inlineDiff->setAccessibleName("Inline diff"); pages->addWidget(inlineDiff);
    pair=new QSplitter(pages); pair->setChildrenCollapsible(false);
    left=new CodePane(pair); left->setObjectName("fileBefore"); left->setAccessibleName("Before");
    right=new CodePane(pair); right->setObjectName("fileAfter"); right->setAccessibleName("After"); pages->addWidget(pair);
    left->resized=[this] { scheduleAlignment(); }; right->resized=left->resized;
    connect(qApp,&QApplication::focusChanged,this,[this](QWidget*,QWidget* focused) {
        if(focused==left || focused==right) searchBefore=focused==left;
    });
    body->setStretchFactor(1,1); body->setSizes({220,900}); layout->addWidget(body,1);
    setFocusProxy(source);
    auto sync=[](QScrollBar* a,QScrollBar* b) { QObject::connect(a,&QScrollBar::valueChanged,b,[b](int value) { QSignalBlocker guard(b); b->setValue(value); }); };
    sync(left->verticalScrollBar(),right->verticalScrollBar()); sync(right->verticalScrollBar(),left->verticalScrollBar());
    sync(left->horizontalScrollBar(),right->horizontalScrollBar()); sync(right->horizontalScrollBar(),left->horizontalScrollBar());
    connect(mode,&QComboBox::currentIndexChanged,this,[this] { if(!refreshing) patch({{"file_mode",mode->currentData().toString()}}); });
    connect(wrap,&QCheckBox::toggled,this,[this](bool value) { if(!refreshing) patch({{"file_wrap",value}}); });
    connect(context,&QCheckBox::toggled,this,[this](bool value) { if(!refreshing) patch({{"file_context",value}}); });
    connect(search,&QLineEdit::returnPressed,this,[this] { find(); });
    connect(search,&QLineEdit::textChanged,this,[this] { searchStatus->clear(); });
    connect(files,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* item) { if(!refreshing && item) { selected=files->indexOfTopLevelItem(item); render(); } });
    connect(filePicker,&QComboBox::currentIndexChanged,this,[this](int index) {
        if(!refreshing && index>=0) { selected=index; QSignalBlocker guard(files); files->setCurrentItem(files->topLevelItem(index)); render(); }
    });
    for(const auto key:{QKeySequence::Find,QKeySequence::FindNext,QKeySequence::FindPrevious}) {
        auto* shortcut=new QShortcut(key,this); shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut,&QShortcut::activated,this,[this,key] {
            if(key==QKeySequence::Find) { search->setFocus(); search->selectAll(); }
            else find(key==QKeySequence::FindPrevious);
        });
    }
    auto* expand=new QShortcut(QKeySequence("Ctrl+Shift+Space"),this); expand->setContext(Qt::WidgetWithChildrenShortcut);
    connect(expand,&QShortcut::activated,this,[this] { context->toggle(); });
    auto* softWrap=new QShortcut(QKeySequence("Alt+Z"),this); softWrap->setContext(Qt::WidgetWithChildrenShortcut);
    connect(softWrap,&QShortcut::activated,this,[this] { if(wrap->isVisible()) wrap->toggle(); });
}
void FileView::setCardWidgets(QWidget* actions,QWidget* status) {
    cardActions=actions;
    actions->setParent(toolbar->parentWidget()); toolbar->addWidget(actions);
    status->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); static_cast<QLabel*>(status)->setWordWrap(true);
    status->setParent(this); static_cast<QVBoxLayout*>(layout())->insertWidget(1,status);
}
void FileView::patch(QJsonObject value) {
    if(playbackSettingsRequested) playbackSettingsRequested(value);
    else { auto settings=config["settings"].toObject(); for(auto it=value.begin();it!=value.end();++it) settings[it.key()]=it.value(); config["settings"]=settings; render(); }
}
bool FileView::reload(const QJsonObject& next,QString& error) {
    FilePreview::Document candidate;
    if(!FilePreview::load(next["path"].toString(),next["before"].toString(),candidate,error)) return false;
    doc=std::move(candidate); path=next["path"].toString(); config=next; ++loads;
    static_cast<MarkdownDocument*>(rendered->document())->invalidateImages();
    lineCount=doc.text.count('\n')+1;
    refreshing=true; files->clear(); filePicker->clear();
    mode->clear();
    if(doc.patch) { mode->addItem("Inline diff","inline"); mode->addItem("Side by side","split"); }
    if(doc.markdown) mode->addItem("Rendered","rendered");
    mode->addItem("Source","source");
    for(const auto& file:doc.changes) {
        const auto name=file.after=="/dev/null"?file.before:file.after;
        auto* item=new QTreeWidgetItem(files,{name+QString("   +%1  -%2").arg(file.added).arg(file.removed)});
        item->setData(0,ChangeName,name); item->setData(0,AddedCount,file.added); item->setData(0,RemovedCount,file.removed);
        item->setToolTip(0,file.before+" → "+file.after);
        filePicker->addItem(item->text(0));
        filePicker->setItemData(filePicker->count()-1,name,ChangeName);
        filePicker->setItemData(filePicker->count()-1,file.added,AddedCount);
        filePicker->setItemData(filePicker->count()-1,file.removed,RemovedCount);
        filePicker->setItemData(filePicker->count()-1,item->toolTip(0),Qt::ToolTipRole);
    }
    selected=std::clamp(selected,0,std::max(0,int(doc.changes.size())-1));
    if(!doc.changes.isEmpty()) files->setCurrentItem(files->topLevelItem(selected));
    refreshing=false; render(); return true;
}
void FileView::settings(const QJsonObject& next) { config=next; render(); }
void FileView::render() {
    refreshing=true;
    int readingPosition=-1,readingAnchor=0,readingVertical=0,readingHorizontal=0;
    const auto settings=config["settings"].toObject();
    const QString desired=settings["file_mode"].toString(doc.patch?"inline":doc.markdown?"rendered":"source");
    mode->setCurrentIndex(std::max(0,mode->findData(desired)));
    wrap->setChecked(settings["file_wrap"].toBool(true)); context->setChecked(settings["file_context"].toBool(false));
    const auto view=mode->currentData().toString();
    wrap->setVisible(view!="rendered"); context->setVisible(doc.patch && view!="source");
    const bool comparing=view=="inline" || view=="split";
    files->setVisible(doc.changes.size()>1 && width()>=760 && comparing);
    filePicker->setVisible(doc.changes.size()>1 && width()<760 && comparing);
    filePicker->setCurrentIndex(selected);
    const int page=view=="rendered"?1:view=="inline"?2:view=="split"?3:0;
    pages->setCurrentIndex(page);
    const ContentStamp stamp{loads,comparing?selected:-1,comparing && context->isChecked()};
    const bool rebuild=prepared[page]!=stamp;
    // A presentation toggle must not reconstruct or highlight other documents.
    // Each page prepares lazily against the complete input/file/context stamp.
    const auto wrapping=wrap->isChecked()?QTextEdit::WidgetWidth:QTextEdit::NoWrap;
    for(auto* editor:page==3?QList<CodePane*>{left,right}:page==2?QList<CodePane*>{inlineDiff}:page==0?QList<CodePane*>{source}:QList<CodePane*>{})
        if(editor->lineWrapMode()!=wrapping) editor->setLineWrapMode(wrapping);
    if(rebuild && page==0) {
        source->emphasis.clear();
        QVector<FilePreview::Row> raw;
        const auto sourceLines=doc.text.split('\n');
        for(int i=0;i<sourceLines.size();++i) raw.append({' ',sourceLines[i],0,i+1});
        source->content(raw,QFileInfo(path).suffix().toLower());
    }
    if(rebuild && page==1) {
        auto* document=static_cast<MarkdownDocument*>(rendered->document());
        const DocumentLayoutBatch batch(document);
        document->root=QFileInfo(path).absolutePath();
        document->setBaseUrl(QUrl::fromLocalFile(document->root+"/"));
        if(rendered->property("markdownSource").toString()!=doc.text || rendered->property("markdownPath").toString()!=path) {
            const bool samePath=rendered->property("markdownPath").toString()==path;
            const auto previous=rendered->textCursor();
            readingPosition=samePath?previous.position():0; readingAnchor=samePath?previous.anchor():0;
            readingVertical=samePath?rendered->verticalScrollBar()->value():0;
            readingHorizontal=samePath?rendered->horizontalScrollBar()->value():0;
            FilePreview::setMarkdown(rendered->document(),doc.text);
            static_cast<MarkdownBrowser*>(rendered)->prepareContent();
            static_cast<MarkdownBrowser*>(rendered)->presentation();
            rendered->setProperty("markdownSource",doc.text); rendered->setProperty("markdownPath",path);
        } else {
            // A same-text save can still replace image bytes or dimensions.
            static_cast<MarkdownBrowser*>(rendered)->refreshImages();
        }
    }
    if(rebuild && comparing && selected<doc.changes.size()) {
        const auto& file=doc.changes[selected];
        QVector<FilePreview::Row> shown;
        for(int i=0;i<file.rows.size();) {
            if(!context->isChecked() && file.rows[i].kind==' ') {
                int end=i; while(end<file.rows.size() && file.rows[end].kind==' ') ++end;
                if(end-i>10) {
                    for(int j=i;j<i+3;++j) shown.append(file.rows[j]);
                    shown.append({'@',QString("⋯ %1 unchanged lines · enable Full context to expand").arg(end-i-6),0,0});
                    for(int j=end-3;j<end;++j) shown.append(file.rows[j]);
                } else for(int j=i;j<end;++j) shown.append(file.rows[j]);
                i=end;
            } else shown.append(file.rows[i++]);
        }
        QVector<FilePreview::Row> before,after;
        QVector<QPair<int,int>> inlineEmphasis(shown.size());
        QVector<QPair<int,int>> beforeEmphasis,afterEmphasis;
        for(int i=0;i<shown.size();) {
            if(shown[i].kind!='-' && shown[i].kind!='+') {
                before.append(shown[i]); before.last().after=0;
                after.append(shown[i]); beforeEmphasis.append({0,0}); afterEmphasis.append({0,0}); ++i; continue;
            }
            QVector<int> removed,added;
            while(i<shown.size() && (shown[i].kind=='-' || shown[i].kind=='+')) {
                (shown[i].kind=='-'?removed:added).append(i);
                ++i;
            }
            for(int j=0;j<std::max(removed.size(),added.size());++j) {
                const auto a=j<removed.size()?shown[removed[j]]:FilePreview::Row{QChar(),"",0,0};
                const auto b=j<added.size()?shown[added[j]]:FilePreview::Row{QChar(),"",0,0};
                before.append(a); after.append(b);
                int prefix=0,suffix=0;
                while(prefix<a.text.size() && prefix<b.text.size() && a.text[prefix]==b.text[prefix]) ++prefix;
                while(suffix<a.text.size()-prefix && suffix<b.text.size()-prefix && a.text[a.text.size()-1-suffix]==b.text[b.text.size()-1-suffix]) ++suffix;
                const QPair<int,int> ae{prefix,int(a.text.size())-prefix-suffix},be{prefix,int(b.text.size())-prefix-suffix};
                beforeEmphasis.append(ae); afterEmphasis.append(be);
                if(j<removed.size()) inlineEmphasis[removed[j]]=ae;
                if(j<added.size()) inlineEmphasis[added[j]]=be;
            }
        }
        const auto extension=QFileInfo(file.after=="/dev/null"?file.before:file.after).suffix().toLower();
        if(page==2) { inlineDiff->emphasis=inlineEmphasis; inlineDiff->content(shown,extension,true); }
        else { left->emphasis=beforeEmphasis; right->emphasis=afterEmphasis; left->content(before,extension); right->content(after,extension); }
    }
    prepared[page]=stamp;
    updateSummary();
    setFocusProxy(view=="rendered"?static_cast<QWidget*>(rendered):view=="inline"?inlineDiff:view=="split"?right:source);
    if(readingPosition>=0) {
        // Parsing advances the live cursor with inserted text; focus transfer
        // can then scroll it into view. Restore numeric positions after both.
        QTextCursor reading(rendered->document());
        const int end=rendered->document()->characterCount()-1;
        reading.setPosition(std::min(readingAnchor,end));
        reading.setPosition(std::min(readingPosition,end),QTextCursor::KeepAnchor);
        rendered->setTextCursor(reading);
        rendered->verticalScrollBar()->setValue(readingVertical);
        rendered->horizontalScrollBar()->setValue(readingHorizontal);
    }
    refreshing=false;
    if(page==3) scheduleAlignment();
}
void FileView::updateSummary() {
    summary->setVisible(!doc.patch || width()>=1000);
    if(doc.patch && selected<doc.changes.size()) {
        const auto& file=doc.changes[selected];
        const auto beforeName=QFileInfo(file.before).fileName(),afterName=QFileInfo(file.after).fileName();
        summary->setText(QString("%1   <span style=\"color:%2\">+%3</span>  <span style=\"color:%4\">-%5</span>")
            .arg((beforeName==afterName?afterName:beforeName+" → "+afterName).toHtmlEscaped(),countColor(palette(),true).name()).arg(file.added)
            .arg(countColor(palette(),false).name()).arg(file.removed));
        summary->setToolTip(file.before+" → "+file.after);
    } else {
        summary->setText(QFileInfo(path).fileName().toHtmlEscaped());
        summary->setToolTip(QString("%1\nUTF-8 · %2 lines · read only").arg(path).arg(lineCount));
    }
}
void FileView::find(bool backwards) {
    if(search->text().isEmpty()) return;
    const auto flags=backwards?QTextDocument::FindBackward:QTextDocument::FindFlags{};
    const auto view=mode->currentData().toString();
    bool found=false;
    if(view=="rendered") {
        found=rendered->find(search->text(),flags);
        if(!found) { rendered->moveCursor(backwards?QTextCursor::End:QTextCursor::Start); found=rendered->find(search->text(),flags); }
    } else {
        auto* editor=view=="inline"?inlineDiff:view=="split"?(searchBefore?left:right):source;
        found=editor->find(search->text(),flags);
        if(!found) { editor->moveCursor(backwards?QTextCursor::End:QTextCursor::Start); found=editor->find(search->text(),flags); }
    }
    searchStatus->setText(found?QString():"No match");
}
void FileView::addNavigationActions(QMenu* menu) {
    const auto view=mode->currentData().toString();
    if(view!="inline" && view!="split") return;
    for(int direction:{-1,1}) {
        auto* action=menu->addAction(direction<0?"Previous change":"Next change");
        connect(action,&QAction::triggered,this,[this,direction] { hunk(direction); });
    }
}
void FileView::hunk(int direction) {
    const auto view=mode->currentData().toString();
    if(view!="inline" && view!="split") return;
    auto* editor=view=="inline"?inlineDiff:right;
    QVector<int> changes;
    auto changed=[&](int index) {
        const auto kind=editor->rows[index].kind;
        return kind=='+' || kind=='-' || (view=="split" && (left->rows[index].kind=='+' || left->rows[index].kind=='-'));
    };
    for(int i=0;i<editor->rows.size();++i) {
        if(changed(i) && (i==0 || !changed(i-1))) changes.append(i);
    }
    if(changes.isEmpty()) return;
    const int current=editor->textCursor().blockNumber();
    int target=direction>0?changes.first():changes.last();
    if(direction>0) { for(int at:changes) if(at>current) { target=at; break; } }
    else for(auto it=changes.crbegin();it!=changes.crend();++it) if(*it<current) { target=*it; break; }
    editor->setTextCursor(QTextCursor(editor->document()->findBlockByNumber(target))); editor->ensureCursorVisible();
}
void FileView::scheduleAlignment() {
    if(alignmentPending || aligning) return;
    alignmentPending=true;
    QTimer::singleShot(0,this,[this] { alignmentPending=false; alignPair(); });
}
void FileView::alignPair() {
    if(mode->currentData()!="split" || aligning) return;
    const QSize widths(left->viewport()->width(),right->viewport()->width());
    const std::array fonts{left->font(),right->font()};
    const bool wrapping=left->lineWrapMode()==QTextEdit::WidgetWidth;
    if(alignedContent==prepared[3] && alignedWidths==widths && alignedFonts==fonts && alignedWrapping==wrapping) return;
    aligning=true;
    const int position=right->verticalScrollBar()->value();
    const QSignalBlocker leftScroll(left->verticalScrollBar()),rightScroll(right->verticalScrollBar());
    // QTextEdit block margins let the shorter wrapped row reserve the same
    // height as its partner without inserting padding into selectable text.
    for(auto* editor:{left,right}) {
        QTextCursor cursor(editor->document()); cursor.beginEditBlock();
        for(auto block=editor->document()->begin();block.isValid();block=block.next()) {
            cursor.setPosition(block.position()); auto format=block.blockFormat();
            if(format.bottomMargin()!=0) { format.setBottomMargin(0); cursor.setBlockFormat(format); }
        }
        cursor.endEditBlock();
    }
    QVector<qreal> padding;
    auto a=left->document()->begin(),b=right->document()->begin();
    while(a.isValid() && b.isValid()) {
        const auto heightA=left->document()->documentLayout()->blockBoundingRect(a).height();
        const auto heightB=right->document()->documentLayout()->blockBoundingRect(b).height();
        padding.append(heightB-heightA);
        a=a.next(); b=b.next();
    }
    // Measure before changing formats so each row does not force a fresh layout
    // of all following rows in a large file.
    for(auto* editor:{left,right}) {
        QTextCursor cursor(editor->document()); cursor.beginEditBlock();
        int index=0;
        for(auto block=editor->document()->begin();block.isValid() && index<padding.size();block=block.next(),++index) {
            const qreal extra=padding[index]*(editor==left?1:-1);
            if(extra>0.5) {
                cursor.setPosition(block.position()); auto format=block.blockFormat();
                format.setBottomMargin(extra); cursor.setBlockFormat(format);
            }
        }
        cursor.endEditBlock();
    }
    left->verticalScrollBar()->setValue(position); right->verticalScrollBar()->setValue(position);
    alignedContent=prepared[3]; alignedWidths=widths; alignedFonts=fonts; alignedWrapping=wrapping;
    aligning=false;
    if(widths!=QSize(left->viewport()->width(),right->viewport()->width())) scheduleAlignment();
}
void FileView::fit() { for(auto* editor:{source,inlineDiff,left,right}) editor->moveCursor(QTextCursor::Start); rendered->moveCursor(QTextCursor::Start); }
QJsonObject FileView::metrics() const {
    int added=0,removed=0; for(const auto& file:doc.changes) { added+=file.added; removed+=file.removed; }
    const auto* markdown=static_cast<MarkdownDocument*>(rendered->document());
    const auto current=mode->currentData().toString();
    const QTextEdit* pane=source;
    if(current=="rendered") pane=rendered;
    else if(current=="inline") pane=inlineDiff;
    else if(current=="split") pane=right;
    return {{"loads",loads},{"file_type",doc.patch?"diff":doc.markdown?"markdown":"text"},{"file_mode",mode->currentData().toString()},
        {"lines",lineCount},{"files",int(doc.changes.size())},{"added",added},{"removed",removed},
        {"selected_file",selected},{"file_wrap",wrap->isChecked()},{"read_only",true},{"render_timer",false},
        {"file_search",QJsonObject{{"focused",search->hasFocus()},{"text",search->text()}}},
        {"file_zoom",static_cast<MarkdownBrowser*>(rendered)->zoom()},
        {"file_image_decodes",markdown->imageDecodes},{"file_image_cache_cost",markdown->imageCacheCost()},
        {"file_scroll_y",pane->verticalScrollBar()->value()},{"file_scroll_max",pane->verticalScrollBar()->maximum()}};
}
void FileView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const bool comparing=mode->currentData()=="inline" || mode->currentData()=="split";
    files->setVisible(doc.changes.size()>1 && width()>=760 && comparing);
    filePicker->setVisible(doc.changes.size()>1 && width()<760 && comparing);
    const bool narrow=width()<900;
    if(narrow && searchTools->parentWidget()!=this) {
        static_cast<QVBoxLayout*>(layout())->insertWidget(1,searchTools);
        searchToolbar->insertWidget(0,context);
        searchToolbar->setContentsMargins(6,0,6,3);
    } else if(!narrow && searchTools->parentWidget()==this) {
        toolbar->insertWidget(cardActions?toolbar->indexOf(cardActions):toolbar->count(),searchTools);
        toolbar->insertWidget(2,context);
        searchToolbar->setContentsMargins(0,0,0,0);
    }
    context->setText(width()<760?"Context":"Full context");
    // Ordinary documents keep their filename beside the mode picker even in
    // Gallery or with the folder explorer open. Diff summaries retain their
    // existing compact-mode picker when the full counts would crowd controls.
    summary->setVisible(!doc.patch || width()>=1000);
    scheduleAlignment();
}
void FileView::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if(event->type()==QEvent::PaletteChange && source) updateSummary();
    if(event->type()==QEvent::FontChange && source) scheduleAlignment();
}
