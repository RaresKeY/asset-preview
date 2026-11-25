#include "file_document.h"
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <algorithm>
#include <limits>

namespace FilePreview {
namespace {
QStringList lines(QString text) {
    text.replace("\r\n","\n");
    auto result=text.split('\n');
    if (text.endsWith('\n') || text.isEmpty()) result.removeLast();
    return result;
}
QString patchPath(QString path) {
    path=path.section('\t',0,0);
    if(path.startsWith('"') && path.endsWith('"')) {
        // Git quotes non-ASCII UTF-8 bytes as octal escapes by default.
        const auto quoted=path.mid(1,path.size()-2).toUtf8(); QByteArray decoded;
        for(int i=0;i<quoted.size();++i) {
            if(quoted[i]!='\\' || i+1==quoted.size()) { decoded+=quoted[i]; continue; }
            const auto c=quoted[++i];
            if(c>='0' && c<='7') {
                int value=c-'0';
                for(int n=0;n<2 && i+1<quoted.size() && quoted[i+1]>='0' && quoted[i+1]<='7';++n) value=value*8+(quoted[++i]-'0');
                decoded+=char(value);
            } else decoded+=c=='t'?'\t':c=='n'?'\n':c=='r'?'\r':c;
        }
        path=QString::fromUtf8(decoded);
    }
    if (path.startsWith("a/") || path.startsWith("b/")) path.remove(0,2);
    return path;
}
}
bool isTextPath(const QString& path) {
    static const QSet<QString> extensions{
        "md","markdown","mdown","txt","text","rst","log","diff","patch",
        "json","jsonl","ndjson","yaml","yml","toml","ini","cfg","conf","csv","tsv",
        "xml","html","htm","css","scss","qss","svg","py","pyi","gd","gdshader",
        "tscn","tres","godot","glsl","vert","frag","h","hpp","hh","c","cc","cpp","cxx",
        "cs","rs","go","java","kt","swift","js","jsx","mjs","ts","tsx","sh","bash",
        "fish","zsh","sql","lua","cmake","env","gitignore","gitattributes","editorconfig"};
    const QFileInfo file(path);
    return extensions.contains(file.suffix().toLower()) ||
        QSet<QString>{"readme","license","licence","copying","changelog","dockerfile","containerfile","makefile","cmakelists.txt",".gitignore",".gitattributes",".editorconfig",".env"}.contains(file.fileName().toLower());
}
bool readText(const QString& path,QString& text,QString& error) {
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) { error="Cannot read text file: "+QFileInfo(path).fileName(); return false; }
    const auto bytes=file.read(MaxBytes+1);
    if (bytes.size()>MaxBytes) { error="Text preview exceeds 2 MiB"; return false; }
    if (bytes.contains('\0')) { error="Binary file cannot be previewed as text"; return false; }
    // This is a complete file buffer, so an unfinished final sequence is an
    // invalid save rather than a streaming decoder's pending next chunk.
    QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::Stateless);
    text=decoder(bytes);
    if (decoder.hasError()) { error="Text preview requires UTF-8"; return false; }
    if (text.count('\n')>MaxLines || std::any_of(text.cbegin(),text.cend(),[](QChar c) { return c.unicode()<32 && c!='\n' && c!='\r' && c!='\t'; })) {
        error="Text preview exceeds 20,000 lines or contains binary control characters"; return false;
    }
    for (const auto& line:QStringView(text).split('\n')) if (line.size()>16384) { error="Text preview line exceeds 16,384 characters"; return false; }
    return true;
}
QVector<Change> parsePatch(const QString& text) {
    QVector<Change> files;
    Change* current=nullptr;
    int old=0,next=0,oldLeft=0,nextLeft=0;
    bool inHunk=false,gitEntry=false;
    const QRegularExpression header("^@@ -(\\d+)(?:,(\\d+))? \\+(\\d+)(?:,(\\d+))? @@");
    const QRegularExpression gitHeader(R"re(^diff --git ("(?:\\.|[^"\\])*"|a/.*?) ("(?:\\.|[^"\\])*"|b/.*)$)re");
    const auto input=lines(text);
    for (int i=0;i<input.size();++i) {
        const auto& line=input[i];
        if (inHunk && (oldLeft>0 || nextLeft>0) && !line.isEmpty() && QString(" +-").contains(line[0])) {
            const auto kind=line[0];
            current->rows.append({kind,line.mid(1),kind=='+'?0:old++,kind=='-'?0:next++});
            if (kind!='+') --oldLeft;
            if (kind!='-') --nextLeft;
            if (kind=='+') ++current->added;
            if (kind=='-') ++current->removed;
            continue;
        }
        // Combined merge hunks have multiple before columns. A two-sided view
        // would omit those edits; preserve the complete patch in Source instead.
        if(line.startsWith("diff --cc ") || line.startsWith("diff --combined ") || line.startsWith("@@@")) return {};
        const auto match=header.match(line);
        if (match.hasMatch() && current) {
            bool valid=true;
            auto number=[&](int group,int fallback) {
                if(match.captured(group).isEmpty()) return qint64(fallback);
                bool ok=false; const auto value=match.captured(group).toLongLong(&ok);
                valid=valid && ok && value<std::numeric_limits<int>::max()-MaxLines;
                return value;
            };
            const auto a=number(1,0),b=number(3,0),ac=number(2,1),bc=number(4,1);
            if(!valid) { inHunk=false; current->rows.append({'@',"Unsupported hunk line range",0,0}); continue; }
            old=int(a); next=int(b); oldLeft=int(ac); nextLeft=int(bc);
            inHunk=true; current->rows.append({'@',line,0,0}); continue;
        }
        const auto git=gitHeader.match(line);
        if(git.hasMatch()) {
            files.append({patchPath(git.captured(1)),patchPath(git.captured(2)),{},0,0});
            current=&files.last(); gitEntry=true; inHunk=false; continue;
        }
        if (line.startsWith("--- ") && i+1<input.size() && input[i+1].startsWith("+++ ")) {
            const auto before=patchPath(line.mid(4)),after=patchPath(input[++i].mid(4));
            if(gitEntry && current) { current->before=before; current->after=after; }
            else { files.append({before,after,{},0,0}); current=&files.last(); }
            gitEntry=false; inHunk=false; continue;
        }
        if (line.startsWith("diff --git ")) { current=nullptr; inHunk=false; gitEntry=false; }
        if (current && line.startsWith("\\ No newline")) current->rows.append({'\\',line,0,0});
        if(current && (line.startsWith("old mode ") || line.startsWith("new mode ") || line.startsWith("new file mode ") ||
            line.startsWith("deleted file mode ") || line.startsWith("similarity index ") || line.startsWith("rename from ") ||
            line.startsWith("rename to ") || line.startsWith("copy from ") || line.startsWith("copy to ") ||
            line.startsWith("Binary files ") || line=="GIT binary patch")) current->rows.append({'@',line,0,0});
    }
    return files;
}
Change compare(const QString& before,const QString& after) {
    const auto a=lines(before),b=lines(after);
    const int n=a.size(),m=b.size();
    Change result;
    // Myers keeps ordinary small edits precise. Bound the trace so a completely
    // rewritten file cannot monopolize the GUI or allocate a quadratic matrix.
    const int limit=std::min(n+m,1200),offset=limit+1;
    QVector<int> frontier(2*limit+3,0);
    QVector<QVector<int>> trace;
    bool found=false;
    for (int d=0;d<=limit;++d) {
        trace.append(frontier);
        for (int k=-d;k<=d;k+=2) {
            int x=(k==-d || (k!=d && frontier[offset+k-1]<frontier[offset+k+1])) ? frontier[offset+k+1] : frontier[offset+k-1]+1;
            int y=x-k;
            while (x<n && y<m && a[x]==b[y]) { ++x; ++y; }
            frontier[offset+k]=x;
            if (x>=n && y>=m) { found=true; break; }
        }
        if (found) break;
    }
    if (found) {
        int x=n,y=m;
        for (int d=trace.size()-1;d>=0;--d) {
            const auto& previous=trace[d]; const int k=x-y;
            const int pk=(k==-d || (k!=d && previous[offset+k-1]<previous[offset+k+1]))?k+1:k-1;
            const int px=previous[offset+pk],py=px-pk;
            while (x>px && y>py) { result.rows.append({' ',a[x-1],x,y}); --x; --y; }
            if (!d) break;
            if (x==px) { result.rows.append({'+',b[y-1],0,y}); --y; ++result.added; }
            else { result.rows.append({'-',a[x-1],x,0}); --x; ++result.removed; }
        }
        std::reverse(result.rows.begin(),result.rows.end());
    } else {
        int prefix=0,suffix=0;
        while (prefix<n && prefix<m && a[prefix]==b[prefix]) ++prefix;
        while (suffix<n-prefix && suffix<m-prefix && a[n-1-suffix]==b[m-1-suffix]) ++suffix;
        for (int i=0;i<prefix;++i) result.rows.append({' ',a[i],i+1,i+1});
        for (int i=prefix;i<n-suffix;++i) { result.rows.append({'-',a[i],i+1,0}); ++result.removed; }
        for (int i=prefix;i<m-suffix;++i) { result.rows.append({'+',b[i],0,i+1}); ++result.added; }
        for (int i=0;i<suffix;++i) result.rows.append({' ',a[n-suffix+i],n-suffix+i+1,m-suffix+i+1});
    }
    if (before.endsWith('\n')!=after.endsWith('\n')) {
        result.rows.append({'@',after.endsWith('\n')?"Final newline added":"Final newline removed",0,0});
    }
    return result;
}
bool load(const QString& path,const QString& before,Document& document,QString& error) {
    Document next;
    if (!readText(path,next.text,error)) return false;
    if (!before.isEmpty()) {
        QString previous;
        if (!readText(before,previous,error)) return false;
        auto diff=compare(previous,next.text); diff.before=before; diff.after=path;
        next.changes.append(diff); next.patch=true;
    } else {
        const auto suffix=QFileInfo(path).suffix().toLower();
        next.markdown=QStringList{"md","markdown","mdown"}.contains(suffix);
        const bool gitPatch=next.text.startsWith("diff --git ") || next.text.startsWith("diff --cc ") || next.text.startsWith("diff --combined ");
        if(gitPatch) next.markdown=false;
        if (suffix=="diff" || suffix=="patch" || gitPatch || next.text.startsWith("--- ")) {
            next.changes=parsePatch(next.text); next.patch=!next.changes.isEmpty();
        }
    }
    document=std::move(next); return true;
}
}
