#include "service.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLocalSocket>
#include <QMimeData>
#include <QPushButton>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTime>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace {
constexpr qsizetype MaxMessage = 1024 * 1024;
QJsonObject success(QJsonObject data = {}) { data.insert("ok", true); return data; }
QJsonObject failure(const QString& error) { return {{"ok", false}, {"error", error}}; }
QString stamp(const QString& path) {
    struct stat s{};
    if (::stat(QFile::encodeName(path).constData(), &s)) return "missing";
    return QString("%1:%2:%3:%4:%5").arg(s.st_ino).arg(s.st_size).arg(s.st_mtim.tv_sec).arg(s.st_mtim.tv_nsec).arg(s.st_mode);
}
bool imagePath(const QString& path) {
    static const QSet<QString> formats{"png","jpg","jpeg","webp","bmp","tif","tiff","svg","exr","hdr"};
    return formats.contains(QFileInfo(path).suffix().toLower());
}
QString absolute(const QString& path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
QByteArray readSmall(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.read(4*1024*1024) : QByteArray{}; }
}

Service::Service(QString sock, QString state) : socketPath(std::move(sock)), statePath(std::move(state)) {
    debounce.setSingleShot(true); debounce.setInterval(220);
    connect(&debounce, &QTimer::timeout, this, &Service::changed);
    connect(&watcher, &QFileSystemWatcher::fileChanged, this, [this] { debounce.start(); });
    connect(&watcher, &QFileSystemWatcher::directoryChanged, this, [this] { debounce.start(); });
    server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server, &QLocalServer::newConnection, this, [this] {
        while (server.hasPendingConnections()) {
            auto* socket = server.nextPendingConnection();
            if (++clients > 64) { --clients; socket->abort(); socket->deleteLater(); continue; }
            socket->setReadBufferSize(MaxMessage+1);
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] { --clients; socket->deleteLater(); });
            QTimer::singleShot(5000, socket, [socket] { socket->abort(); socket->deleteLater(); });
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                if (socket->property("answered").toBool()) return;
                if (socket->bytesAvailable() > MaxMessage) { socket->abort(); return; }
                if (!socket->canReadLine()) return;
                socket->setProperty("answered", true);
                QJsonParseError error;
                auto doc = QJsonDocument::fromJson(socket->readLine(MaxMessage), &error);
                auto response = error.error == QJsonParseError::NoError && doc.isObject()
                    ? request(doc.object()) : failure("Invalid JSON object");
                socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
                socket->disconnectFromServer();
            });
        }
    });
}
Service::~Service() { for (auto& e : entries) stopBuilder(*e); window.reset(); }
bool Service::start(QString& error) {
    QFile state(statePath + "/previews.json");
    if (state.exists()) {
        if (!state.open(QIODevice::ReadOnly) || state.size() > 4*1024*1024) { error="Cannot read preview state"; return false; }
        QJsonParseError parse;
        auto doc=QJsonDocument::fromJson(state.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject() || doc.object()["version"].toInt() != 1 || !doc.object()["entries"].isArray()) {
            error="Invalid preview state; preserved at " + state.fileName(); return false;
        }
        for (const auto& row : doc.object()["entries"].toArray()) {
            auto result=add(row.toObject(), true);
            if (!result["ok"].toBool()) { error="Invalid saved preview: " + result["error"].toString(); return false; }
        }
        layout=doc.object()["layout"].toString("single");
        if (layout != "single" && layout != "grid") { error="Invalid saved layout"; return false; }
        selected=doc.object()["selected"].toString();
    }
    if (!entries.contains(selected)) selected=order.value(0);
    QLocalServer::removeServer(socketPath);
    if (!server.listen(socketPath)) { error=server.errorString(); return false; }
    return true;
}
void Service::save() {
    QJsonArray rows;
    for (const auto& id : order) rows.append(entries[id]->config);
    QSaveFile file(statePath + "/previews.json");
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error(file.errorString().toStdString());
    file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    auto data=QJsonDocument(QJsonObject{{"version",1},{"entries",rows},{"selected",selected},{"layout",layout}}).toJson();
    if (file.write(data) != data.size() || !file.commit()) throw std::runtime_error("Cannot save preview registry");
}
QString Service::validate(QJsonObject& c, bool restore) {
    if (QJsonDocument(c).toJson(QJsonDocument::Compact).size()>8192) return "Preview configuration exceeds 8 KiB";
    QString path=c["path"].toString();
    if (path.isEmpty() || !QFileInfo(path).isAbsolute()) return "An absolute output file path is required";
    if (!restore && QFileInfo(path).isDir()) return "Output must be a file";
    if (!restore && QFileInfo(path).size() > 256*1024*1024) return "Preview file exceeds 256 MiB";
    c["path"]=absolute(path);
    QString kind=c["kind"].toString("auto");
    if (kind == "auto") kind=imagePath(path)?"image":"model";
    if (kind != "image" && kind != "model" && kind != "material") return "Kind must be image, model or material";
    c["kind"]=kind;
    QString id=c["id"].toString();
    if (id.isEmpty()) id=QUuid::createUuid().toString(QUuid::Id128).left(12);
    if (!QRegularExpression("^[A-Za-z0-9_-]{1,64}$").match(id).hasMatch()) return "ID must use letters, digits, underscores or dashes (max 64)";
    c["id"]=id;
    QString label=c["label"].toString(QFileInfo(path).fileName());
    if (label.size()>200) return "Label exceeds 200 characters";
    c["label"]=label;
    const auto command=c.value("command");
    if (!command.isUndefined() && !command.isArray()) return "command must be an argument array";
    if (command.toArray().size()>128) return "Too many command arguments";
    for (const auto& a : command.toArray()) if (!a.isString() || a.toString().isEmpty()) return "Command arguments must be nonempty strings";
    if (!command.toArray().isEmpty() && (!QFileInfo(c["cwd"].toString()).isAbsolute() || (!restore && !QFileInfo(c["cwd"].toString()).isDir()))) return "Generator requires an existing absolute cwd";
    if (c.contains("watch") && !c.value("watch").isArray()) return "watch must be a path array";
    if (c["watch"].toArray().size()>128) return "Too many watched source paths";
    QJsonArray sources;
    for (const auto& a : c["watch"].toArray()) {
        if (!a.isString() || !QFileInfo(a.toString()).isAbsolute() || (!restore && QFileInfo(a.toString()).isDir())) return "Watch paths must be absolute files (missing files are allowed)";
        sources.append(absolute(a.toString()));
    }
    c["watch"]=sources;
    auto maps=c.value("maps").toObject();
    for (auto i=maps.begin();i!=maps.end();++i) {
        if ((i.key()!="normal" && i.key()!="orm") || !i.value().isString() || !QFileInfo(i.value().toString()).isAbsolute()) return "Maps support absolute normal and ORM paths";
    }
    if (c.contains("maps") && !c.value("maps").isObject()) return "maps must be an object";
    if (c.contains("settings") && !c.value("settings").isObject()) return "settings must be an object";
    auto settings=c.value("settings").toObject();
    const QSet<QString> known{"grid","axes","edges","orthographic","nearest","light","background","shape","roughness","metallic"};
    for (auto i=settings.begin();i!=settings.end();++i) {
        if (!known.contains(i.key())) return "Unknown setting: " + i.key();
        if (i.key()=="background") { if (!QSet<QString>{"dark","light","checker"}.contains(i.value().toString())) return "Invalid background"; }
        else if (i.key()=="shape") { if (!QSet<QString>{"sphere","cube","plane"}.contains(i.value().toString())) return "Invalid material shape"; }
        else if (i.key()=="light" || i.key()=="roughness" || i.key()=="metallic") {
            if (!i.value().isDouble() || i.value().toDouble()<0 || i.value().toDouble()>(i.key()=="light"?5.0:1.0)) return "Invalid numeric setting";
        } else if (!i.value().isBool()) return "Boolean setting required: " + i.key();
    }
    if (c.contains("timeout") && (!c.value("timeout").isDouble() || c.value("timeout").toDouble()<1 || c.value("timeout").toDouble()>3600)) return "Timeout must be 1–3600 seconds";
    c["command"]=command.toArray(); c["maps"]=maps; c["settings"]=settings;
    c["timeout"]=c.value("timeout").toDouble(120);
    return {};
}
QJsonObject Service::add(QJsonObject config, bool restore) {
    const QString error=validate(config,restore);
    if (!error.isEmpty()) return failure(error);
    const QString id=config["id"].toString();
    if (!entries.contains(id) && entries.size()>=256) return failure("Preview limit reached (256)");
    auto prior=entries.value(id);
    const bool existing=bool(prior);
    QJsonObject old=prior?prior->config:QJsonObject{};
    if (!prior) { prior=std::make_shared<Entry>(); entries[id]=prior; order.append(id); }
    prior->config=config;
    if (selected.isEmpty()) selected=id;
    if (!restore) {
        try { save(); }
        catch (const std::exception& e) {
            if (existing) prior->config=old;
            else { entries.remove(id); order.removeAll(id); if (selected==id) selected=order.value(0); }
            return failure(e.what());
        }
        if (existing) deactivate(id);
        reconcile();
    }
    return success({{"id",id}});
}
QJsonObject Service::describe(const Entry& e) const {
    QJsonObject result=e.config;
    result["active"]=e.active; result["status"]=e.status;
    result["builds"]=e.builds; result["revisions"]=e.revisions;
    result["building"]=bool(e.process && e.process->state()!=QProcess::NotRunning);
    result["log"]=e.log;
    if (e.view) result["metrics"]=e.view->metrics();
    if (e.surface && window) {
        const auto position=e.surface->mapTo(window.get(),QPoint());
        result["viewport"]=QJsonObject{{"x",position.x()},{"y",position.y()},{"width",e.surface->width()},{"height",e.surface->height()}};
    }
    return result;
}
QJsonObject Service::request(const QJsonObject& r) {
    try {
        const QString method=r["method"].toString(), id=r["id"].toString();
        if (method=="ping") return success({{"pid",qint64(QCoreApplication::applicationPid())},{"protocol",1},{"platform",QGuiApplication::platformName()}});
        if (method=="list") {
            QJsonArray rows; int active=0;
            for (const auto& key:order) { rows.append(describe(*entries[key])); active+=entries[key]->active; }
            return success({{"entries",rows},{"selected",selected},{"layout",layout},{"active",active},
                {"window_visible",bool(window && window->isVisible())},{"watched_files",watcher.files().size()},
                {"watched_directories",watcher.directories().size()},{"pid",qint64(QCoreApplication::applicationPid())},
                {"platform",QGuiApplication::platformName()}});
        }
        if (method=="add") return add(r["entry"].toObject());
        if (method=="show") {
            if (!window) window=std::make_unique<PreviewWindow>(this);
            window->showNormal(); window->raise(); window->activateWindow(); reconcile(); return success();
        }
        if (method=="hide") { if (window) window->hide(); reconcile(); return success(); }
        if (method=="quit") { QTimer::singleShot(0,qApp,&QCoreApplication::quit); return success(); }
        if (method=="layout") {
            QString next=r["layout"].toString();
            if (next!="single" && next!="grid") return failure("Layout must be single or grid");
            layout=next; save(); reconcile(); return success();
        }
        if (method=="capture") {
            const QString path=r["path"].toString();
            if (!window || !window->isVisible()) return failure("Window must be open to capture");
            if (!QFileInfo(path).isAbsolute() || QFileInfo(path).exists()) return failure("Capture requires a new absolute file path");
            QImage capture=window->grab().toImage();
            QPainter painter(&capture);
            for (const auto& e:entries) if (e->active) {
                if (e->config["kind"].toString()!="image" && e->view) {
                    const auto image=e->view->snapshot();
                    painter.drawImage(QRect(e->surface->mapTo(window.get(),QPoint()),e->surface->size()),image);
                }
            }
            painter.end();
            if (!capture.save(path,"PNG")) return failure("Cannot save capture");
            return success();
        }
        if (!entries.contains(id)) return failure("Unknown preview: " + id);
        auto e=entries[id];
        if (method=="remove") {
            deactivate(id); entries.remove(id); order.removeAll(id);
            if (selected==id) selected=order.value(0);
            save(); reconcile(); return success();
        }
        if (method=="select") { selected=id; save(); reconcile(); return success(); }
        if (method=="reload") { if (e->active) { if (!e->config["command"].toArray().isEmpty()) build(id); else reload(id); } return success(); }
        if (method=="settings") {
            auto config=e->config, settings=config["settings"].toObject();
            const auto patch=r["settings"].toObject();
            if (!r["settings"].isObject()) return failure("settings must be an object");
            for (auto i=patch.begin();i!=patch.end();++i) settings[i.key()]=i.value();
            config["settings"]=settings; QString error=validate(config);
            if (!error.isEmpty()) return failure(error);
            const auto old=e->config; e->config=config;
            try { save(); } catch (...) { e->config=old; throw; }
            if (e->view) e->view->settings(config);
            return success();
        }
        return failure("Unknown method");
    } catch (const std::exception& e) { return failure(QString::fromUtf8(e.what())); }
}
void Service::status(Entry& e, const QString& text) {
    e.status=text;
    if (e.statusLabel) { e.statusLabel->setText(text.left(140)); e.statusLabel->setToolTip(text + (e.log.isEmpty()?"":"\n\n"+e.log)); }
}
QStringList Service::dependencies(const Entry& e) const {
    QStringList paths{e.config["path"].toString()};
    for (const auto& p:e.config["watch"].toArray()) paths.append(p.toString());
    for (const auto& p:e.config["maps"].toObject()) paths.append(p.toString());
    const QString path=e.config["path"].toString(), suffix=QFileInfo(path).suffix().toLower();
    const QDir dir=QFileInfo(path).absoluteDir();
    auto uri=[&](const QString& value) {
        QUrl u(value);
        if (!value.isEmpty() && u.isRelative() && !value.startsWith("data:")) paths.append(absolute(dir.filePath(QUrl::fromPercentEncoding(value.toUtf8()))));
    };
    if (suffix=="gltf" || suffix=="glb") {
        QByteArray data=readSmall(path);
        if (suffix=="glb" && data.size()>=20 && data.left(4)=="glTF") {
            // GLB's first chunk is the JSON resource description.
            const auto* b=reinterpret_cast<const unsigned char*>(data.constData()+12);
            const quint32 length=quint32(b[0])|(quint32(b[1])<<8)|(quint32(b[2])<<16)|(quint32(b[3])<<24);
            data=data.mid(20,std::min<quint32>(length,data.size()-20));
        }
        const auto root=QJsonDocument::fromJson(data).object();
        for (const auto& p:root["images"].toArray()) uri(p.toObject()["uri"].toString());
        for (const auto& p:root["buffers"].toArray()) uri(p.toObject()["uri"].toString());
    } else if (suffix=="obj") {
        const auto text=QString::fromUtf8(readSmall(path));
        auto matches=QRegularExpression("^\\s*mtllib\\s+(.+)$",QRegularExpression::MultilineOption).globalMatch(text);
        while (matches.hasNext()) {
            const QString mtl=absolute(dir.filePath(matches.next().captured(1).trimmed())); paths.append(mtl);
            auto maps=QRegularExpression("^\\s*(?:map_\\w+|bump|disp|norm)\\s+(.+)$",QRegularExpression::MultilineOption).globalMatch(QString::fromUtf8(readSmall(mtl)));
            while (maps.hasNext()) {
                QString name=maps.next().captured(1).trimmed();
                if (name.startsWith('-')) name=QProcess::splitCommand(name).value(QProcess::splitCommand(name).size()-1);
                paths.append(absolute(QFileInfo(mtl).absoluteDir().filePath(name)));
            }
        }
    }
    paths.removeDuplicates();
    return paths.mid(0,256);
}
void Service::watchers() {
    QSet<QString> files, dirs;
    for (const auto& e:entries) if (e->active) {
        const auto paths=dependencies(*e);
        QMap<QString,QString> retained;
        for (const auto& p:paths) {
            retained[p]=e->stamps.contains(p)?e->stamps[p]:stamp(p);
            if (QFileInfo(p).isFile()) files.insert(p);
            QString parent=QFileInfo(p).absolutePath();
            while (!QFileInfo(parent).isDir() && parent!="/") parent=QFileInfo(parent).absolutePath();
            dirs.insert(parent);
        }
        e->stamps=retained;
    }
    const auto currentFiles=watcher.files(), currentDirs=watcher.directories();
    for (const auto& p:currentFiles) if (!files.contains(p)) watcher.removePath(p);
    for (const auto& p:currentDirs) if (!dirs.contains(p)) watcher.removePath(p);
    QStringList add;
    for (const auto& p:files) if (!currentFiles.contains(p)) add.append(p);
    for (const auto& p:dirs) if (!currentDirs.contains(p)) add.append(p);
    if (!add.isEmpty()) {
        const auto rejected=watcher.addPaths(add);
        if (!rejected.isEmpty()) qWarning("Some watches could not be installed: %s",qPrintable(rejected.join(", ")));
    }
}
void Service::changed() {
    for (const auto& id:order) {
        auto e=entries[id]; if (!e->active) continue;
        bool dirty=false, source=false, output=false;
        QSet<QString> sourcePaths;
        for (const auto& p:e->config["watch"].toArray()) sourcePaths.insert(p.toString());
        for (auto i=e->stamps.begin();i!=e->stamps.end();++i) {
            const auto next=stamp(i.key());
            if (next!=i.value()) {
                dirty=true; source|=sourcePaths.contains(i.key()); output|=!sourcePaths.contains(i.key()); i.value()=next;
            }
        }
        if (dirty) {
            ++e->token; e->retries=0;
            if (source && !e->config["command"].toArray().isEmpty()) build(id);
            if (output || !source || e->config["command"].toArray().isEmpty()) reload(id);
        }
    }
    watchers();
}
void Service::reload(const QString& id) {
    auto e=entries.value(id); if (!e || !e->active || !e->view) return;
    QString error;
    if (QFileInfo(e->config["path"].toString()).size()>256*1024*1024) { status(*e,"File exceeds 256 MiB"); return; }
    const bool building=e->process && e->process->state()!=QProcess::NotRunning;
    if (e->view->reload(e->config,error)) {
        ++e->revisions; e->retries=0;
        status(*e,(building?"Building · preview updated ":"Live · refreshed ") + QTime::currentTime().toString("HH:mm:ss"));
    }
    else {
        status(*e,(building?"Building · waiting · ":"Waiting · ") + error);
        if (++e->retries<=3) {
            const auto token=e->token;
            QTimer::singleShot(200*(1<<e->retries),this,[this,id,token] {
                const auto e=entries.value(id); if (e && e->active && e->token==token) reload(id);
            });
        }
    }
}
void Service::stopBuilder(Entry& e) {
    if (e.deadline) e.deadline->stop();
    if (e.process) {
        disconnect(e.process.get(),nullptr,this,nullptr);
        const auto pid=e.process->property("groupPid").toLongLong();
        if (pid>0) {
            ::kill(-pid,SIGTERM);
            e.process->waitForFinished(150);
            ::kill(-pid,SIGKILL);
            if (e.process->state()!=QProcess::NotRunning) { e.process->kill(); e.process->waitForFinished(150); }
        }
        e.process.reset();
    }
    e.rerun=false;
}
void Service::build(const QString& id) {
    auto e=entries.value(id); if (!e || !e->active || !e->view) return;
    if (e->process && e->process->state()!=QProcess::NotRunning) { e->rerun=true; return; }
    const auto args=e->config["command"].toArray(); if (args.isEmpty()) { reload(id); return; }
    stopBuilder(*e);
    e->process=std::make_unique<QProcess>(); e->deadline=std::make_unique<QTimer>();
    e->deadline->setSingleShot(true); e->log.clear(); ++e->builds;
    auto* p=e->process.get();
    p->setWorkingDirectory(e->config["cwd"].toString()); p->setProcessChannelMode(QProcess::MergedChannels);
    p->setChildProcessModifier([] { if (::setsid()<0) _exit(126); });
    QStringList arguments; for (int i=1;i<args.size();++i) arguments.append(args[i].toString());
    connect(p,&QProcess::readyReadStandardOutput,this,[this,id] {
        auto e=entries.value(id); if (!e || !e->process) return;
        e->log=(e->log + QString::fromUtf8(e->process->readAllStandardOutput())).right(16384);
        status(*e,"Building · " + e->log.trimmed().section('\n',-1).left(110));
    });
    connect(p,&QProcess::errorOccurred,this,[this,id](QProcess::ProcessError error) {
        auto e=entries.value(id); if (!e || !e->process) return;
        if (error==QProcess::FailedToStart) { e->deadline->stop(); status(*e,"Generator failed · " + e->process->errorString()); }
    });
    connect(p,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this,id](int code,QProcess::ExitStatus exit) {
        auto e=entries.value(id); if (!e || !e->active || !e->process) return;
        e->deadline->stop();
        // Retire descendants too, even if a generator exits before them.
        const auto pid=e->process->property("groupPid").toLongLong();
        if (pid>0) { ::kill(-pid,SIGTERM); ::kill(-pid,SIGKILL); e->process->setProperty("groupPid",0); }
        e->log=(e->log+QString::fromUtf8(e->process->readAllStandardOutput())).right(16384);
        if (e->rerun) { e->rerun=false; QTimer::singleShot(0,this,[this,id] { build(id); }); return; }
        if (code==0 && exit==QProcess::NormalExit) {
            reload(id);
            QSet<QString> sources;
            for (const auto& p:e->config["watch"].toArray()) sources.insert(p.toString());
            for (const auto& p:dependencies(*e)) if (!sources.contains(p)) e->stamps[p]=stamp(p);
            watchers();
        }
        else status(*e,"Generator failed · exit " + QString::number(code));
    });
    connect(p,&QProcess::started,this,[this,id] {
        auto e=entries.value(id); if (!e || !e->process) return;
        e->process->setProperty("groupPid",e->process->processId());
        e->deadline->start(int(e->config["timeout"].toDouble(120)*1000));
    });
    connect(e->deadline.get(),&QTimer::timeout,this,[this,id] {
        auto e=entries.value(id); if (!e) return;
        stopBuilder(*e); status(*e,"Generator timed out");
    });
    status(*e,"Building · waiting for output");
    p->start(args[0].toString(),arguments);
}
void Service::activate(const QString& id) {
    auto e=entries[id]; if (e->active) return;
    e->active=true; e->stamps.clear(); ++e->token;
    window->createCard(*e);
    reload(id);
    if (!e->config["command"].toArray().isEmpty()) build(id);
}
void Service::deactivate(const QString& id) {
    auto e=entries.value(id); if (!e || !e->active) return;
    e->active=false; ++e->token;
    stopBuilder(*e);
    if (window) window->removeCard(*e);
    e->view=nullptr; e->surface=nullptr; e->statusLabel=nullptr; e->card=nullptr; e->stamps.clear();
    status(*e,"Suspended");
}
void Service::reconcile() {
    bool wasActive=false;
    for (const auto& e:entries) wasActive|=e->active;
    QStringList visible;
    if (window && window->isVisible() && !window->isMinimized() && !order.isEmpty()) {
        int i=std::max(0,int(order.indexOf(selected)));
        visible=layout=="grid" ? order.mid((i/4)*4,4) : order.mid(i,1);
    }
    for (const auto& id:order) if (!visible.contains(id)) deactivate(id);
    for (const auto& id:visible) activate(id);
    if (window) { window->arrange(visible); window->navigation(); }
    watchers();
#if defined(__GLIBC__)
    if (wasActive && visible.isEmpty()) ::malloc_trim(0);
#endif
}
void Service::navigate(int delta) {
    if (order.isEmpty()) return;
    int index=std::max(0,int(order.indexOf(selected)));
    if (layout=="grid") index=std::clamp((index/4+delta)*4,0,int(order.size())-1);
    else index=std::clamp(index+delta,0,int(order.size())-1);
    selected=order[index]; save(); reconcile();
}

PreviewWindow::PreviewWindow(Service* s) : service(s) {
    setWindowTitle("Asset Preview"); setWindowIcon(QIcon(QStringLiteral(ASSET_PREVIEW_ROOT "/icon.svg")));
    setMinimumSize(540,380); resize(1000,720); setAcceptDrops(true);
    auto* central=new QWidget; auto* vertical=new QVBoxLayout(central); vertical->setContentsMargins(16,12,16,12); vertical->setSpacing(12);
    auto* top=new QHBoxLayout;
    auto* title=new QLabel("Asset Preview"); auto font=title->font(); font.setPointSize(15); font.setBold(true); title->setFont(font);
    top->addWidget(title); top->addStretch();
    auto* addButton=new QPushButton("Add files"); addButton->setMinimumHeight(36); top->addWidget(addButton);
    mode=new QPushButton; mode->setMinimumHeight(36); top->addWidget(mode);
    vertical->addLayout(top);
    auto* controls=new QHBoxLayout;
    previous=new QPushButton("←"); next=new QPushButton("→"); previous->setFixedSize(40,36); next->setFixedSize(40,36);
    previous->setToolTip("Previous preview / page (Alt+Left)"); next->setToolTip("Next preview / page (Alt+Right)");
    selection=new QComboBox; selection->setMinimumHeight(36); selection->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    selection->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); selection->setMinimumContentsLength(12);
    position=new QLabel; position->setMinimumWidth(65); position->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    controls->addWidget(previous); controls->addWidget(selection,1); controls->addWidget(next); controls->addWidget(position);
    vertical->addLayout(controls);
    canvas=new QWidget; grid=new QGridLayout(canvas); grid->setContentsMargins(0,0,0,0); grid->setSpacing(12);
    vertical->addWidget(canvas,1); setCentralWidget(central);
    statusBar()->showMessage("Live on save · drag files here · Alt+Left / Right to switch · close to suspend");
    statusBar()->setToolTip("Local socket: " + service->socketPath);
    connect(addButton,&QPushButton::clicked,this,[this] {
        for (const auto& path:QFileDialog::getOpenFileNames(this,"Add asset previews",QDir::homePath(),"Assets (*.png *.jpg *.jpeg *.webp *.bmp *.svg *.glb *.gltf *.obj *.stl *.ply *.fbx);;All files (*)"))
            service->request({{"method","add"},{"entry",QJsonObject{{"path",path}}}});
    });
    connect(mode,&QPushButton::clicked,this,[this] { service->request({{"method","layout"},{"layout",service->layout=="single"?"grid":"single"}}); });
    connect(previous,&QPushButton::clicked,this,[this] { service->navigate(-1); });
    connect(next,&QPushButton::clicked,this,[this] { service->navigate(1); });
    connect(selection,&QComboBox::activated,this,[this](int index) { service->request({{"method","select"},{"id",selection->itemData(index).toString()}}); });
    auto* left=new QShortcut(QKeySequence("Alt+Left"),this); connect(left,&QShortcut::activated,this,[this] { service->navigate(-1); });
    auto* right=new QShortcut(QKeySequence("Alt+Right"),this); connect(right,&QShortcut::activated,this,[this] { service->navigate(1); });
    QSettings settings(service->statePath+"/window.ini",QSettings::IniFormat);
    restoreGeometry(settings.value("geometry").toByteArray());
}
void PreviewWindow::navigation() {
    QSignalBlocker block(selection); selection->clear();
    for (const auto& id:service->order) selection->addItem(service->entries[id]->config["label"].toString(),id);
    selection->setCurrentIndex(service->order.indexOf(service->selected));
    const int i=std::max(0,int(service->order.indexOf(service->selected))), n=service->order.size();
    previous->setEnabled(n>0 && (service->layout=="grid"?i/4>0:i>0));
    next->setEnabled(n>0 && (service->layout=="grid"?(i/4+1)*4<n:i+1<n));
    mode->setText(service->layout=="single"?"Grid view":"Single view");
    position->setText(n==0?"0 / 0":service->layout=="single"?QString("%1 / %2").arg(i+1).arg(n):QString("%1–%2 / %3").arg((i/4)*4+1).arg(std::min((i/4+1)*4,n)).arg(n));
}
void PreviewWindow::arrange(const QStringList& ids) {
    while (auto* item=grid->takeAt(0)) { if (item->widget() && item->widget()->property("empty").toBool()) delete item->widget(); delete item; }
    for (int i=0;i<2;++i) { grid->setRowStretch(i,0); grid->setColumnStretch(i,0); }
    if (ids.isEmpty()) {
        auto* empty=new QLabel("Watch your work take shape\n\nAdd or drop an image, model, or exported material.\nPreviews refresh when the files are saved.");
        empty->setProperty("empty",true); empty->setAlignment(Qt::AlignCenter); empty->setWordWrap(true);
        grid->addWidget(empty,0,0); grid->setRowStretch(0,1); grid->setColumnStretch(0,1);
    } else {
        for (int i=0;i<ids.size();++i) { const int cols=service->layout=="grid" && ids.size()>1?2:1;
            grid->addWidget(service->entries[ids[i]]->card,i/cols,i%cols); grid->setRowStretch(i/cols,1); grid->setColumnStretch(i%cols,1); }
    }
}
void PreviewWindow::createCard(Entry& e) {
    const QString id=e.config["id"].toString();
    e.card=new QFrame(canvas); e.card->setFrameShape(QFrame::StyledPanel);
    auto* vertical=new QVBoxLayout(e.card); vertical->setContentsMargins(10,8,10,8); vertical->setSpacing(6);
    auto* header=new QHBoxLayout;
    auto* label=new QLabel(e.config["label"].toString()); label->setToolTip(e.config["path"].toString());
    label->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); auto font=label->font(); font.setBold(true); label->setFont(font);
    header->addWidget(label,1);
    auto* fit=new QPushButton("Fit"); auto* settings=new QPushButton("Options"); auto* remove=new QPushButton("×");
    fit->setToolTip("Fit camera / image (also double click)"); remove->setToolTip("Remove this preview"); remove->setFixedWidth(30);
    header->addWidget(fit); header->addWidget(settings); header->addWidget(remove); vertical->addLayout(header);
    e.statusLabel=new QLabel(e.status); e.statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setTextFormat(Qt::PlainText); e.statusLabel->setTextFormat(Qt::PlainText);
    e.statusLabel->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    QWidget* widget=nullptr;
    if (e.config["kind"].toString()=="image") { auto* image=new ImageView(e.card); e.view=image; widget=image; }
    else {
        auto loaded=[this,id](bool ok,QString error) {
            const auto e=service->entries.value(id); if (!e || !e->active) return;
            if (e->process && e->process->state()!=QProcess::NotRunning) return;
            e->status=ok?"Live · model ready":"Waiting · "+error;
            if (e->statusLabel) { e->statusLabel->setText(e->status); e->statusLabel->setToolTip(e->status); }
        };
        try {
            const auto handle=createModel(e.config,e.card,std::move(loaded));
            e.view=handle.view; widget=handle.surface;
        } catch (const std::exception& error) {
            e.status="3D backend unavailable · " + QString::fromUtf8(error.what());
            qWarning("%s",qPrintable(e.status));
            e.statusLabel->setText(e.status); e.statusLabel->setToolTip(e.status);
            auto* message=new QLabel("Build the 3D backend with tools/build.sh, then reopen this preview.",e.card);
            message->setWordWrap(true); message->setAlignment(Qt::AlignCenter); widget=message;
        }
    }
    e.surface=widget;
    vertical->addWidget(widget,1); vertical->addWidget(e.statusLabel);
    widget->setToolTip(e.config["kind"].toString()=="image"?"Scroll to zoom · drag to pan · double click to fit":"Drag to orbit · right / middle drag to pan · scroll to zoom · double click to fit");
    connect(fit,&QPushButton::clicked,this,[this,id] { const auto e=service->entries.value(id); if (e && e->view) e->view->fit(); });
    connect(settings,&QPushButton::clicked,this,[this,id] { options(id); });
    connect(remove,&QPushButton::clicked,this,[this,id] { service->request({{"method","remove"},{"id",id}}); });
}
void PreviewWindow::removeCard(Entry& e) { if (e.card) { grid->removeWidget(e.card); delete e.card; } }
void PreviewWindow::options(const QString& id) {
    auto e=service->entries.value(id); if (!e) return;
    QDialog dialog(this); dialog.setWindowTitle("Preview options");
    auto* form=new QFormLayout(&dialog); auto settings=e->config["settings"].toObject();
    auto* background=new QComboBox; background->addItems({"dark","light","checker"});
    background->setCurrentText(settings["background"].toString(e->config["kind"].toString()=="image"?"checker":"dark"));
    form->addRow("Background",background);
    QMap<QString,QCheckBox*> checks;
    for (const auto& name : e->config["kind"].toString()=="image"?QStringList{"nearest"}:QStringList{"grid","axes","edges","orthographic"}) {
        auto* check=new QCheckBox; check->setChecked(settings[name].toBool(name=="grid")); checks[name]=check;
        form->addRow(name=="nearest"?"Pixel filtering":name=="edges"?"Show edges":name=="axes"?"Axis indicator":name=="grid"?"Ground grid":"Orthographic camera",check);
    }
    QDoubleSpinBox* light=nullptr; QComboBox* shape=nullptr;
    if (e->config["kind"].toString()!="image") { light=new QDoubleSpinBox; light->setRange(0,5); light->setSingleStep(.1); light->setValue(settings["light"].toDouble(1)); form->addRow("Light intensity",light); }
    if (e->config["kind"].toString()=="material") { shape=new QComboBox; shape->addItems({"sphere","cube","plane"}); shape->setCurrentText(settings["shape"].toString("sphere")); form->addRow("Sample shape",shape); }
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); form->addRow(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if (dialog.exec()==QDialog::Accepted) {
        settings["background"]=background->currentText(); for (auto i=checks.begin();i!=checks.end();++i) settings[i.key()]=i.value()->isChecked();
        if (light) settings["light"]=light->value();
        if (shape) settings["shape"]=shape->currentText();
        service->request({{"method","settings"},{"id",id},{"settings",settings}});
    }
}
void PreviewWindow::closeEvent(QCloseEvent* event) {
    QSettings settings(service->statePath+"/window.ini",QSettings::IniFormat); settings.setValue("geometry",saveGeometry());
    hide(); service->reconcile(); event->ignore();
}
void PreviewWindow::changeEvent(QEvent* e) {
    QMainWindow::changeEvent(e); if (e->type()==QEvent::WindowStateChange) QTimer::singleShot(0,service,&Service::reconcile);
}
void PreviewWindow::dragEnterEvent(QDragEnterEvent* e) { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
void PreviewWindow::dropEvent(QDropEvent* e) {
    for (const auto& url:e->mimeData()->urls()) if (url.isLocalFile()) service->request({{"method","add"},{"entry",QJsonObject{{"path",url.toLocalFile()}}}});
    e->acceptProposedAction();
}
