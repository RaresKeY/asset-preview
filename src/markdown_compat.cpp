#include "markdown_compat.h"
#include <md4c.h>
#include <QMap>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextImageFormat>
#include <QUuid>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace {
struct Tag {
    qsizetype offset=0,length=0;
    bool image=false;
    QMap<QString,QString> attributes;
};
struct Scan {
    QByteArray source;
    QVector<Tag> tags;
    int imageDepth=0;
    bool htmlBlock=false,firstHtml=false,convertBlock=false;
    QString terminator,rawElement;
    bool continuedTag=false;
    QChar continuedQuote;
    QString continuedName;
};
QString attributeText(const QString& bytes) {
    const auto value=QString::fromUtf8(bytes.toLatin1());
    static const QRegularExpression entity(R"re(&(?:#[0-9]{1,8}|#[xX][0-9a-fA-F]{1,8}|[A-Za-z][A-Za-z0-9]{0,31});)re");
    QString result; qsizetype at=0;
    auto matches=entity.globalMatch(value);
    while(matches.hasNext()) {
        const auto match=matches.next(); result+=QStringView(value).mid(at,match.capturedStart()-at);
        // Only a matched entity reaches the HTML utility, never attribute markup
        // or a resource reference. This also handles named HTML entities.
        result+=QTextDocumentFragment::fromHtml(match.captured()).toPlainText(); at=match.capturedEnd();
    }
    result+=QStringView(value).mid(at); return result;
}
int enterBlock(MD_BLOCKTYPE type,void*,void* data) {
    auto& scan=*static_cast<Scan*>(data);
    if(type==MD_BLOCK_HTML) {
        scan.htmlBlock=true; scan.firstHtml=true; scan.convertBlock=false;
        scan.terminator.clear(); scan.rawElement.clear(); scan.continuedTag=false;
    }
    return 0;
}
int leaveBlock(MD_BLOCKTYPE type,void*,void* data) {
    if(type==MD_BLOCK_HTML) {
        auto& scan=*static_cast<Scan*>(data); scan.htmlBlock=false;
        scan.terminator.clear(); scan.rawElement.clear(); scan.continuedTag=false;
    }
    return 0;
}
int enterSpan(MD_SPANTYPE type,void*,void* data) {
    if(type==MD_SPAN_IMG)++static_cast<Scan*>(data)->imageDepth;
    return 0;
}
int leaveSpan(MD_SPANTYPE type,void*,void* data) {
    if(type==MD_SPAN_IMG)--static_cast<Scan*>(data)->imageDepth;
    return 0;
}
int text(MD_TEXTTYPE type,const MD_CHAR* value,MD_SIZE length,void* data) {
    auto& scan=*static_cast<Scan*>(data);
    if(type!=MD_TEXT_HTML || scan.imageDepth)return 0;
    // HTML callbacks reference original UTF-8 bytes. Latin-1 preserves byte
    // offsets while matching ASCII syntax; attribute values decode afterward.
    const auto html=QString::fromLatin1(value,int(length));
    static const QRegularExpression start(R"re(^\s*<(?:br|img)(?=[\s/>]))re",QRegularExpression::CaseInsensitiveOption);
    if(scan.htmlBlock && scan.firstHtml) { scan.convertBlock=start.match(html).hasMatch(); scan.firstHtml=false; }
    if((scan.htmlBlock && !scan.convertBlock) || (!scan.htmlBlock && !start.match(html).hasMatch()))return 0;
    static const QRegularExpression attributes(R"re(([A-Za-z_:][A-Za-z0-9_.:-]*)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+)))re");
    qsizetype at=0;
    while(at<html.size()) {
        // Block HTML arrives as opaque lines, including comments and raw-text
        // elements. Lex whole tags before considering their names, so neither
        // nested examples nor a custom <img-widget> becomes a native image.
        if(scan.continuedTag) {
            for(;at<html.size();++at) {
                const auto ch=html[at];
                if(!scan.continuedQuote.isNull()) { if(ch==scan.continuedQuote)scan.continuedQuote={}; }
                else if(ch=='\'' || ch=='"')scan.continuedQuote=ch;
                else if(ch=='>')break;
            }
            if(at==html.size())return 0;
            ++at; scan.continuedTag=false;
            if(scan.continuedName=="script" || scan.continuedName=="pre" || scan.continuedName=="style" || scan.continuedName=="textarea")scan.rawElement=scan.continuedName;
        }
        if(!scan.terminator.isEmpty()) {
            const auto end=html.indexOf(scan.terminator,at);
            if(end<0)return 0;
            at=end+scan.terminator.size(); scan.terminator.clear();
        }
        if(!scan.rawElement.isEmpty()) {
            const QRegularExpression close("</"+scan.rawElement+"\\s*>",QRegularExpression::CaseInsensitiveOption);
            const auto end=close.match(html,at);
            if(!end.hasMatch())return 0;
            at=end.capturedEnd(); scan.rawElement.clear();
        }
        const auto begin=html.indexOf('<',at);
        if(begin<0)break;
        at=begin+1;
        const auto tail=QStringView(html).mid(begin);
        if(tail.startsWith(u"<!--")) { scan.terminator="-->"; at=begin+4; continue; }
        if(tail.startsWith(u"<![CDATA[")) { scan.terminator="]]>"; at=begin+9; continue; }
        if(tail.startsWith(u"<?")) { scan.terminator="?>"; at=begin+2; continue; }
        if(tail.startsWith(u"<!")) { scan.terminator=">"; at=begin+2; continue; }
        const bool closing=at<html.size() && html[at]=='/';
        if(closing)++at;
        const auto nameStart=at;
        while(at<html.size() && !html[at].isSpace() && html[at]!='/' && html[at]!='>')++at;
        const auto name=html.mid(nameStart,at-nameStart).toLower();
        const auto attributesStart=at;
        QChar quote;
        for(;at<html.size();++at) {
            const auto ch=html[at];
            if(!quote.isNull()) { if(ch==quote)quote={}; }
            else if(ch=='\'' || ch=='"')quote=ch;
            else if(ch=='>')break;
        }
        if(at==html.size()) {
            // An opaque block's tag may span callback lines. Keep it literal,
            // including apparent tags inside any continued attribute value.
            scan.continuedTag=true; scan.continuedQuote=quote;
            scan.continuedName=closing?QString():name;
            break;
        }
        ++at;
        if(closing)continue;
        if(name=="script" || name=="pre" || name=="style" || name=="textarea") { scan.rawElement=name; continue; }
        if(name!="br" && name!="img")continue;
        Tag tag;
        tag.offset=(value-scan.source.constData())+begin; tag.length=at-begin;
        tag.image=name=="img";
        if(tag.image) {
            auto fields=attributes.globalMatch(html.mid(attributesStart,at-attributesStart-1));
            while(fields.hasNext()) {
                const auto field=fields.next(); const auto key=field.captured(1).toLower();
                if(tag.attributes.contains(key) || (key!="src" && key!="alt" && key!="title" && key!="width" && key!="height"))continue;
                for(int i=2;i<=4;++i)if(field.capturedStart(i)>=0) { tag.attributes[key]=attributeText(field.captured(i)); break; }
            }
            if(tag.attributes.value("src").isEmpty())continue;
        }
        scan.tags.append(std::move(tag));
    }
    return 0;
}
}

void FilePreview::setMarkdown(QTextDocument* document,const QString& source) {
    const auto flags=QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML;
    // Ordinary Markdown keeps the existing single-parser path.
    static const QRegularExpression candidate(R"re(<(?:br|img)\b)re",QRegularExpression::CaseInsensitiveOption);
    if(!source.contains(candidate)) { document->setMarkdown(source,flags); return; }
    Scan scan; scan.source=source.toUtf8();
    MD_PARSER parser{}; parser.flags=MD_FLAG_TABLES|MD_FLAG_STRIKETHROUGH|MD_FLAG_TASKLISTS|MD_FLAG_PERMISSIVEAUTOLINKS;
    parser.enter_block=enterBlock; parser.leave_block=leaveBlock; parser.enter_span=enterSpan; parser.leave_span=leaveSpan; parser.text=text;
    if(md_parse(scan.source.constData(),MD_SIZE(scan.source.size()),&parser,&scan)!=0 || scan.tags.isEmpty()) {
        document->setMarkdown(source,flags); return;
    }
    const auto prefix=QString(QChar(0xe000))+"aphtml"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto suffix=QChar(0xe001); const auto prefixBytes=prefix.toUtf8();
    QByteArray adapted; adapted.reserve(scan.source.size()); qsizetype at=0;
    for(qsizetype i=0;i<scan.tags.size();++i) {
        const auto& tag=scan.tags[i]; adapted.append(scan.source.constData()+at,tag.offset-at);
        adapted+=prefixBytes+QByteArray::number(i)+QString(suffix).toUtf8(); at=tag.offset+tag.length;
    }
    adapted.append(scan.source.constData()+at,scan.source.size()-at);
    document->setMarkdown(QString::fromUtf8(adapted),flags);
    // QTextDocument::find copies a block's text on each call. Repeated searches
    // in a long paragraph are quadratic. Scan each block once, then replace
    // backwards so the saved document positions remain valid.
    struct Replacement { int first,last; qsizetype tag; };
    QVector<Replacement> replacements;
    for(auto block=document->begin();block.isValid();block=block.next()) {
        const auto text=block.text(); qsizetype at=0;
        while((at=text.indexOf(prefix,at))>=0) {
            const auto numberStart=at+prefix.size(),end=text.indexOf(suffix,numberStart);
            if(end<0)break;
            bool valid=false; const auto index=QStringView(text).mid(numberStart,end-numberStart).toLongLong(&valid);
            if(valid && index>=0 && index<scan.tags.size())
                replacements.append({block.position()+int(at),block.position()+int(end)+1,qsizetype(index)});
            at=end+1;
        }
    }
    QTextCursor edit(document); edit.beginEditBlock();
    QTextCursor cursor(document);
    for(auto it=replacements.crbegin();it!=replacements.crend();++it) {
        const auto& tag=scan.tags[it->tag]; cursor.setPosition(it->first); cursor.setPosition(it->last,QTextCursor::KeepAnchor);
        if(!tag.image)cursor.insertText(QString(QChar::LineSeparator));
        else {
            QTextImageFormat image; image.merge(cursor.charFormat());
            image.setObjectType(QTextFormat::ImageObject);
            image.setName(tag.attributes.value("src"));
            image.setProperty(QTextFormat::ImageAltText,tag.attributes.value("alt"));
            image.setProperty(QTextFormat::ImageTitle,tag.attributes.value("title"));
            for(const auto* name:{"width","height"}) {
                bool ok=false; const double size=tag.attributes.value(name).toDouble(&ok);
                if(ok && std::isfinite(size) && size>0) {
                    if(name[0]=='w')image.setWidth(std::min(size,1600.)); else image.setHeight(std::min(size,1600.));
                }
            }
            cursor.insertImage(image);
        }
    }
    edit.endEditBlock();
}
