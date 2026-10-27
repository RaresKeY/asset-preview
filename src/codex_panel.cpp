#include "codex_panel.h"
#include <QApplication>
#include <QFileInfo>
#include <QLabel>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <sys/stat.h>
#include <unistd.h>
#if ASSET_PREVIEW_HAS_TERMINAL
#include <KParts/ReadOnlyPart>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <kde_terminal_interface.h>
#endif

QString validateCodexOrigin(const QJsonValue& value) {
    if (!value.isObject()) return "codex must be an object";
    const auto c=value.toObject();
    static const QRegularExpression uuid("^[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$");
    if (!c["thread_id"].isString() || !uuid.match(c["thread_id"].toString()).hasMatch()) return "Invalid Codex thread ID";
    for (const auto& key:{"cwd","socket"}) {
        const auto path=c[key].toString();
        if (!c[key].isString() || path.size()>4096 || !QFileInfo(path).isAbsolute()) return "Codex paths must be absolute";
        for (auto ch:path) if (ch.category()==QChar::Other_Control) return "Control character in Codex path";
    }
    for (auto it=c.begin();it!=c.end();++it)
        if (it.key()!="thread_id" && it.key()!="cwd" && it.key()!="socket") return "Unknown Codex origin field";
    return {};
}
CodexPanel::CodexPanel(QWidget* parent):QWidget(parent),layout(new QVBoxLayout(this)) {
    setObjectName("codexPanel"); setMinimumWidth(300);
    layout->setContentsMargins(0,0,0,0);
    status=new QLabel("Connect an asset from Codex to open its conversation here.");
    status->setTextFormat(Qt::PlainText); status->setWordWrap(true); status->setMargin(12);
    layout->addWidget(status);
}
CodexPanel::~CodexPanel() { detach(); }
bool CodexPanel::attach(const QJsonObject& next) {
#if ASSET_PREVIEW_HAS_TERMINAL
    const auto running=[](QObject* object) {
        auto* terminal=object?qobject_cast<TerminalInterface*>(object):nullptr;
        return terminal && terminal->terminalProcessId()>0;
    };
    if (part && terminalWidget && !exited && origin==next && running(part)) { terminalWidget->setFocus(); return true; }
#endif
    const auto invalid=validateCodexOrigin(next);
    if (!invalid.isEmpty()) { status->setText(next.isEmpty()?"This asset has no originating Codex conversation. Reconnect it from that conversation first.":invalid); status->show(); return false; }
    struct stat info{};
    const auto socket=next["socket"].toString();
    if (::stat(socket.toLocal8Bit().constData(),&info)!=0 || !S_ISSOCK(info.st_mode) || info.st_uid!=getuid() || (info.st_mode & 0077)) {
        status->setText("The originating Codex server is unavailable or its socket is not private to this user."); status->show(); return false;
    }
    const auto executable=QStandardPaths::findExecutable("codex");
    if (executable.isEmpty()) { status->setText("Codex is not installed in PATH."); return false; }
    const auto environment=QStandardPaths::findExecutable("env");
    if (environment.isEmpty()) { status->setText("The terminal environment launcher (env) is unavailable."); return false; }
    if (!QFileInfo(next["cwd"].toString()).isDir()) { status->setText("The conversation's working folder is unavailable."); return false; }
#if ASSET_PREVIEW_HAS_TERMINAL
    // Tab queues and unsent drafts belong to this installed TUI, not the
    // server. Park clients when switching instead of discarding their inputs.
    if (part) {
        if (terminalWidget) terminalWidget->hide();
        retained.append({part,partIdentity,terminalWidget,origin,exited || !running(part)});
        part=nullptr; partIdentity=nullptr; terminalWidget=nullptr; origin={};
    }
    for (qsizetype i=0;i<retained.size();++i) if (retained[i].origin==next && retained[i].part && retained[i].widget && !retained[i].exited && running(retained[i].part)) {
        const auto session=retained.takeAt(i);
        part=session.part; partIdentity=session.identity; terminalWidget=session.widget; origin=session.origin; exited=false;
        status->hide(); terminalWidget->show(); terminalWidget->setFocus(); return true;
    }
    // Reap exited frontends only. Never evict a live TUI with pending inputs.
    for (qsizetype i=retained.size();i>0;--i) if (!retained[i-1].part || retained[i-1].exited || !running(retained[i-1].part)) {
        auto session=retained.takeAt(i-1); if(session.part) session.part->disconnect(this); delete session.part.data();
    }
    if (retained.size()>=8) { status->setText("Eight conversation terminals are retained. Reopen one of those conversations to preserve their pending inputs."); status->show(); return false; }
    const auto loaded=KPluginFactory::instantiatePlugin<KParts::ReadOnlyPart>(KPluginMetaData("kf6/parts/konsolepart"),this);
    auto* terminal=loaded.plugin?qobject_cast<TerminalInterface*>(loaded.plugin):nullptr;
    if (!terminal) { delete loaded.plugin; status->setText("Konsole component unavailable: "+loaded.errorString); status->show(); return false; }
    part=loaded.plugin; partIdentity=part.data(); origin=next; terminalWidget=loaded.plugin->widget(); exited=false;
    // Konsole's terminalExited() is an internal slot that deletes the part.
    // Watch destruction using its saved identity: QPointer is already cleared
    // when QObject::destroyed is delivered, including for parked clients.
    connect(part.data(),&QObject::destroyed,this,&CodexPanel::terminalExited);
    terminalWidget->setObjectName("codexTerminal"); layout->addWidget(terminalWidget,1); status->hide();
    // Explicit remote attachment never starts or owns a server. Destroying the
    // terminal closes its PTY/client only; no interrupt or shutdown RPC is sent.
    // Agent command runners disable colors in their own output. This child has
    // a real Konsole PTY: describe it accurately without changing the service's
    // environment or the settings inherited by generators and other children.
    terminal->startProgram(environment,{environment,"-u","NO_COLOR","-u","FORCE_COLOR","-u","CLICOLOR","-u","CLICOLOR_FORCE",
        "TERM=xterm-256color","COLORTERM=truecolor",executable,
        "--remote","unix://"+socket,"resume",next["thread_id"].toString(),"--cd",next["cwd"].toString()});
    terminalWidget->setFocus(); return true;
#else
    status->setText("Terminal support was not built (KF6 Parts / Konsole is optional)."); return false;
#endif
}
void CodexPanel::detach() {
    if (part) part->disconnect(this);
    delete part.data(); part=nullptr; partIdentity=nullptr; terminalWidget=nullptr; origin={};
    for (auto& session:retained) { if(session.part) session.part->disconnect(this); delete session.part.data(); }
    retained.clear();
    status->show();
}
void CodexPanel::terminalExited(QObject* terminal) {
    if (terminal!=partIdentity) {
        for (auto& session:retained) if (session.identity==terminal) { session.exited=true; if (session.widget) session.widget->hide(); }
        return;
    }
    exited=true; partIdentity=nullptr;
    if (terminalWidget) terminalWidget->hide();
    terminalWidget=nullptr;
    status->setText("Codex terminal disconnected. Reopen the sidebar to reconnect to the same conversation; its background work remains on the Codex server.");
    status->show();
}
bool CodexPanel::pasteImage(const QString& path,const QString& context) {
#if ASSET_PREVIEW_HAS_TERMINAL
    auto* terminal=part?qobject_cast<TerminalInterface*>(part.data()):nullptr;
    if (!terminal || exited || terminal->terminalProcessId()<=0) return false;
    for (auto ch:path) if (ch.category()==QChar::Other_Control) return false;
    // Codex interprets an explicit bracketed paste of a local image path as
    // an image attachment. Never send Enter: the operator owns submission.
    terminal->sendInput("\x1b[200~"+path+"\x1b[201~");
    auto safeContext=context.left(8192);
    for (qsizetype i=0;i<safeContext.size();++i) { const QChar ch=safeContext[i]; if (ch.category()==QChar::Other_Control && ch!='\n' && ch!='\t') safeContext[i]=' '; }
    if (!safeContext.isEmpty()) terminal->sendInput("\x1b[200~\n"+safeContext+"\n\x1b[201~");
    terminalWidget->setFocus(); return true;
#else
    Q_UNUSED(path); Q_UNUSED(context); return false;
#endif
}
bool CodexPanel::ownsFocus() const {
    const auto* focused=QApplication::focusWidget();
    return focused && (focused==this || isAncestorOf(focused));
}
QJsonObject CodexPanel::describe(QWidget* relative) const {
    int pid=0;
#if ASSET_PREVIEW_HAS_TERMINAL
    if (auto* terminal=part && !exited?qobject_cast<TerminalInterface*>(part.data()):nullptr) pid=terminal->terminalProcessId();
#endif
    const auto at=mapTo(relative,QPoint());
    return {{"visible",isVisible()},{"connected",pid>0},{"client_pid",pid},{"retained_clients",retained.size()},{"thread_id",origin["thread_id"]},
        {"x",at.x()},{"y",at.y()},{"width",width()},{"height",height()}};
}
