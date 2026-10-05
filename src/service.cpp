#include "service.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLocalSocket>
#include <QMimeData>
#include <QMenu>
#include <QResizeEvent>
#include <QShowEvent>
#include <QToolButton>
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

class PreviewCard final : public QFrame {
public:
    PreviewCard(QWidget* parent, const QString& title, const QString& path) : QFrame(parent), fullTitle(title) {
        setFrameShape(QFrame::StyledPanel); setMinimumSize(100,100);
        content=new QVBoxLayout(this); content->setContentsMargins(0,0,0,0); content->setSpacing(0);
        header=new QFrame(this); header->setObjectName("viewOverlay");
        // Small native overlays share the container's stacking context with the
        // embedded OpenGL window. A full-window overlay would intercept orbiting.
        header->setAttribute(Qt::WA_NativeWindow);
        header->setStyleSheet("QFrame#viewOverlay {background:#20262e;border:1px solid #414b57;border-radius:4px;}"
            "QFrame#viewOverlay QLabel {color:#e8edf3;background:transparent;border:none;}"
            "QToolButton {color:#e8edf3;background:transparent;border:1px solid transparent;border-radius:3px;padding:2px;}"
            "QToolButton:hover {background:#394453;} QToolButton:pressed {background:#485566;}"
            "QToolButton:focus {border-color:#82d9c8;}");
        auto* row=new QHBoxLayout(header); row->setContentsMargins(7,1,3,1); row->setSpacing(3);
        name=new QLabel(title,header); name->setTextFormat(Qt::PlainText); name->setToolTip(title+"\n"+path);
        name->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        auto font=name->font(); font.setBold(true); name->setFont(font); row->addWidget(name,1);
        fit=new QToolButton(header); fit->setText("Fit"); fit->setToolTip("Fit camera / image (also double click)");
        options=new QToolButton(header); options->setText("⋯"); options->setToolTip("Preview options");
        remove=new QToolButton(header); remove->setText("×"); remove->setToolTip("Remove this preview");
        for (auto* button:{fit,options,remove}) { button->setFixedSize(32,28); button->setFocusPolicy(Qt::StrongFocus); row->addWidget(button); }
        footer=new QLabel(this); footer->setTextFormat(Qt::PlainText); footer->setAttribute(Qt::WA_NativeWindow);
        footer->setTextInteractionFlags(Qt::TextSelectableByMouse);
        footer->setStyleSheet("color:#e8edf3;background:#20262e;border:1px solid #414b57;border-radius:3px;padding:2px 5px;");
    }
    void setSurface(QWidget* surface) { content->addWidget(surface); }
    void setCompact(bool value) {
        compact=value; fit->setVisible(!value); remove->setVisible(!value);
        updateStatus(); place();
    }
    void updateStatus() { footer->setVisible(!compact || !footer->text().startsWith("Live")); place(); }
    void compose(QPainter& painter, QWidget* window) {
        for (auto* widget: {static_cast<QWidget*>(header),static_cast<QWidget*>(footer)})
            if (widget->isVisible()) painter.drawPixmap(widget->mapTo(window,QPoint()),widget->grab());
    }
    QToolButton *fit, *options, *remove;
    QLabel* footer;
protected:
    void resizeEvent(QResizeEvent* event) override { QFrame::resizeEvent(event); place(); }
    void showEvent(QShowEvent* event) override {
        QFrame::showEvent(event); QTimer::singleShot(0,this,[this] { place(); });
    }
private:
    void place() {
        const int inset=compact?4:6;
        header->setGeometry(inset,inset,std::max(1,width()-2*inset),compact?30:34);
        header->layout()->activate();
        name->setText(name->fontMetrics().elidedText(fullTitle,Qt::ElideRight,std::max(0,name->width())));
        footer->setGeometry(inset,std::max(inset,height()-inset-24),std::max(1,width()-2*inset),24);
        header->raise(); footer->raise();
    }
    QString fullTitle;
    QFrame* header;
    QLabel* name;
    QVBoxLayout* content;
    bool compact=false;
};
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
        gridSize=doc.object()["grid_size"].toInt(2);
        if (gridSize<2 || gridSize>4 || (doc.object().contains("grid_size") && doc.object()["grid_size"].toDouble()!=gridSize)) { error="Invalid saved grid size"; return false; }
        if (doc.object().contains("compact") && !doc.object()["compact"].isBool()) { error="Invalid saved compact mode"; return false; }
        compact=doc.object()["compact"].toBool(false);
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
    auto data=QJsonDocument(QJsonObject{{"version",1},{"entries",rows},{"selected",selected},{"layout",layout},{"grid_size",gridSize},{"compact",compact}}).toJson();
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
    const QSet<QString> known{"grid","axes","edges","orthographic","nearest","light","background","shape","roughness","metallic","materials","textures","lock_horizon","up_axis","lighting"};
    for (auto i=settings.begin();i!=settings.end();++i) {
        if (!known.contains(i.key())) return "Unknown setting: " + i.key();
        if (i.key()=="background") { if (!QSet<QString>{"dark","light","checker"}.contains(i.value().toString())) return "Invalid background"; }
        else if (i.key()=="shape") { if (!QSet<QString>{"sphere","cube","plane"}.contains(i.value().toString())) return "Invalid material shape"; }
        else if (i.key()=="up_axis") { if (!QSet<QString>{"y","z"}.contains(i.value().toString())) return "Up axis must be y or z"; }
        else if (i.key()=="lighting") { if (!QSet<QString>{"studio","lightkit"}.contains(i.value().toString())) return "Lighting must be studio or lightkit"; }
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
    if (e.optionsButton && window) {
        const auto p=e.optionsButton->mapTo(window.get(),QPoint());
        result["controls"]=QJsonObject{{"options",QJsonObject{{"x",p.x()},{"y",p.y()},{"width",e.optionsButton->width()},{"height",e.optionsButton->height()}}}};
        result["status_visible"]=e.statusLabel->isVisible();
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
            return success({{"entries",rows},{"selected",selected},{"layout",layout},{"active",active},{"grid_size",gridSize},{"compact",compact},
                {"window_visible",bool(window && window->isVisible())},{"watched_files",watcher.files().size()},
                {"watched_directories",watcher.directories().size()},{"pid",qint64(QCoreApplication::applicationPid())},
                {"platform",QGuiApplication::platformName()},{"options_open",bool(QApplication::activePopupWidget())}});
        }
        if (method=="add") return add(r["entry"].toObject());
        if (method=="show") {
            if (!window) window=std::make_unique<PreviewWindow>(this);
            window->showNormal(); window->raise(); window->activateWindow(); reconcile(); return success();
        }
        if (method=="hide") { if (window) window->hide(); reconcile(); return success(); }
        if (method=="quit") { QTimer::singleShot(0,qApp,&QCoreApplication::quit); return success(); }
        if (method=="layout") {
            if (r.contains("layout") && !r["layout"].isString()) return failure("layout must be a string");
            QString next=r["layout"].toString(layout);
            if (next!="single" && next!="grid") return failure("Layout must be single or grid");
            const int size=r["grid_size"].toInt(gridSize);
            if (r.contains("grid_size") && (!r["grid_size"].isDouble() || r["grid_size"].toDouble()!=size || size<2 || size>4)) return failure("Grid size must be 2, 3 or 4");
            if (r.contains("compact") && !r["compact"].isBool()) return failure("compact must be boolean");
            const auto previous=layout; const auto previousSize=gridSize; const auto previousCompact=compact;
            layout=next; gridSize=size; compact=r["compact"].toBool(compact);
            try { save(); } catch (...) { layout=previous; gridSize=previousSize; compact=previousCompact; throw; }
            reconcile(); return success();
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
            for (const auto& e:entries) if (e->active && e->card) static_cast<PreviewCard*>(e->card)->compose(painter,window.get());
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
    if (e.card) static_cast<PreviewCard*>(e.card)->updateStatus();
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
    e->view=nullptr; e->surface=nullptr; e->statusLabel=nullptr; e->optionsButton=nullptr; e->card=nullptr; e->stamps.clear();
    status(*e,"Suspended");
}
void Service::reconcile() {
    bool wasActive=false;
    for (const auto& e:entries) wasActive|=e->active;
    QStringList visible;
    if (window && window->isVisible() && !window->isMinimized() && !order.isEmpty()) {
        int i=std::max(0,int(order.indexOf(selected)));
        const int count=pageSize(); visible=order.mid((i/count)*count,count);
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
    if (layout=="grid") index=std::clamp((index/pageSize()+delta)*pageSize(),0,int(order.size())-1);
    else index=std::clamp(index+delta,0,int(order.size())-1);
    selected=order[index]; save(); reconcile();
}

PreviewWindow::PreviewWindow(Service* s) : service(s) {
    setWindowTitle("Asset Preview"); setWindowIcon(QIcon(QStringLiteral(ASSET_PREVIEW_ROOT "/icon.svg")));
    setMinimumSize(540,380); resize(1000,720); setAcceptDrops(true);
    auto* central=new QWidget; auto* vertical=new QVBoxLayout(central); vertical->setContentsMargins(8,6,8,6); vertical->setSpacing(6);
    auto* addButton=new QPushButton("Add files"); addButton->setMinimumHeight(32);
    auto* controls=new QHBoxLayout;
    previous=new QPushButton("←"); next=new QPushButton("→"); previous->setFixedSize(32,32); next->setFixedSize(32,32);
    previous->setToolTip("Previous preview / page (Alt+Left)"); next->setToolTip("Next preview / page (Alt+Right)");
    selection=new QComboBox; selection->setMinimumHeight(32); selection->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    selection->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); selection->setMinimumContentsLength(3);
    density=new QComboBox; density->addItems({"Single","2×2","3×3","4×4"}); density->setMinimumHeight(32);
    density->setToolTip("Visible previews per page: 1, 4, 9 or 16");
    compactButton=new QCheckBox("Compact"); compactButton->setToolTip("Tight spacing; keep names and options inside views; show only busy/error status");
    position=new QLabel; position->setMinimumWidth(65); position->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    controls->addWidget(previous); controls->addWidget(selection,1); controls->addWidget(next); controls->addWidget(position);
    controls->addWidget(density); controls->addWidget(compactButton); controls->addWidget(addButton);
    vertical->addLayout(controls);
    canvas=new QWidget; grid=new QGridLayout(canvas); grid->setContentsMargins(0,0,0,0); grid->setSpacing(6);
    vertical->addWidget(canvas,1); setCentralWidget(central);
    statusBar()->showMessage("Drag files to add · Alt+Left / Right to switch · close to suspend");
    connect(addButton,&QPushButton::clicked,this,[this] {
        for (const auto& path:QFileDialog::getOpenFileNames(this,"Add asset previews",QDir::homePath(),"Assets (*.png *.jpg *.jpeg *.webp *.bmp *.svg *.glb *.gltf *.obj *.stl *.ply *.fbx);;All files (*)"))
            service->request({{"method","add"},{"entry",QJsonObject{{"path",path}}}});
    });
    connect(density,&QComboBox::activated,this,[this](int index) {
        QJsonObject request{{"method","layout"},{"layout",index==0?"single":"grid"}};
        if (index>0) request["grid_size"]=index+1;
        service->request(request);
    });
    connect(compactButton,&QCheckBox::toggled,this,[this](bool value) { service->request({{"method","layout"},{"compact",value}}); });
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
    const int count=service->pageSize();
    previous->setEnabled(n>0 && i/count>0);
    next->setEnabled(n>0 && (i/count+1)*count<n);
    QSignalBlocker densityBlock(density), compactBlock(compactButton);
    density->setCurrentIndex(service->layout=="single"?0:service->gridSize-1);
    compactButton->setChecked(service->compact);
    position->setText(n==0?"0 / 0":service->layout=="single"?QString("%1 / %2").arg(i+1).arg(n):QString("%1–%2 / %3").arg((i/count)*count+1).arg(std::min((i/count+1)*count,n)).arg(n));
}
void PreviewWindow::arrange(const QStringList& ids) {
    while (auto* item=grid->takeAt(0)) { if (item->widget() && item->widget()->property("empty").toBool()) delete item->widget(); delete item; }
    for (int i=0;i<4;++i) { grid->setRowStretch(i,0); grid->setColumnStretch(i,0); }
    const int inset=service->compact?4:8;
    centralWidget()->layout()->setContentsMargins(inset,service->compact?4:6,inset,service->compact?4:6);
    centralWidget()->layout()->setSpacing(service->compact?4:6);
    grid->setSpacing(service->compact?2:6); statusBar()->setVisible(!service->compact);
    if (ids.isEmpty()) {
        auto* empty=new QLabel("Watch your work take shape\n\nAdd or drop an image, model, or exported material.\nPreviews refresh when the files are saved.");
        empty->setProperty("empty",true); empty->setAlignment(Qt::AlignCenter); empty->setWordWrap(true);
        grid->addWidget(empty,0,0); grid->setRowStretch(0,1); grid->setColumnStretch(0,1);
    } else {
        for (int i=0;i<ids.size();++i) { const int cols=service->layout=="grid"?std::min(service->gridSize,int(ids.size())):1;
            static_cast<PreviewCard*>(service->entries[ids[i]]->card)->setCompact(service->compact);
            grid->addWidget(service->entries[ids[i]]->card,i/cols,i%cols); grid->setRowStretch(i/cols,1); grid->setColumnStretch(i%cols,1); }
    }
}
void PreviewWindow::createCard(Entry& e) {
    const QString id=e.config["id"].toString();
    auto* card=new PreviewCard(canvas,e.config["label"].toString(),e.config["path"].toString());
    e.card=card; e.statusLabel=card->footer; e.optionsButton=card->options;
    service->status(e,e.status);
    QWidget* widget=nullptr;
    if (e.config["kind"].toString()=="image") { auto* image=new ImageView(e.card); e.view=image; widget=image; }
    else {
        auto loaded=[this,id](bool ok,QString error) {
            const auto e=service->entries.value(id); if (!e || !e->active) return;
            if (e->process && e->process->state()!=QProcess::NotRunning) return;
            service->status(*e,ok?"Live · model ready":"Waiting · "+error);
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
    card->setSurface(widget);
    widget->setToolTip(e.config["kind"].toString()=="image"?"Scroll to zoom · drag to pan · double click to fit":"Drag to orbit · right / middle drag to pan · scroll to zoom · double click to fit");
    connect(card->fit,&QToolButton::clicked,this,[this,id] { const auto e=service->entries.value(id); if (e && e->view) e->view->fit(); });
    connect(card->options,&QToolButton::clicked,this,[this,id] { options(id); });
    connect(card->remove,&QToolButton::clicked,this,[this,id] { service->request({{"method","remove"},{"id",id}}); });
}
void PreviewWindow::removeCard(Entry& e) { if (e.card) { grid->removeWidget(e.card); delete e.card; } }
void PreviewWindow::options(const QString& id) {
    auto e=service->entries.value(id); if (!e) return;
    QMenu menu(this); const auto settings=e->config["settings"].toObject();
    menu.addAction(e->config["label"].toString())->setEnabled(false);
    connect(menu.addAction("Fit view"),&QAction::triggered,this,[e] { if (e->view) e->view->fit(); });
    connect(menu.addAction("Reload / rebuild"),&QAction::triggered,this,[this,id] { service->request({{"method","reload"},{"id",id}}); });
    menu.addSeparator();
    auto patch=[this,id](QJsonObject fields) {
        auto result=service->request({{"method","settings"},{"id",id},{"settings",fields}});
        if (!result["ok"].toBool()) statusBar()->showMessage(result["error"].toString(),5000);
    };
    auto toggle=[&](const QString& label,const QString& key,bool defaultValue=false) {
        auto* action=menu.addAction(label); action->setCheckable(true); action->setChecked(settings[key].toBool(defaultValue));
        connect(action,&QAction::toggled,this,[patch,key](bool value) { patch({{key,value}}); }); return action;
    };
    auto choice=[&](const QString& label,const QString& key,const QStringList& labels,const QStringList& values,const QString& fallback) {
        auto* sub=menu.addMenu(label);
        for (int i=0;i<values.size();++i) {
            auto* action=sub->addAction(labels[i]); action->setCheckable(true); action->setChecked(settings[key].toString(fallback)==values[i]);
            connect(action,&QAction::triggered,this,[patch,key,value=values[i]] { patch({{key,value}}); });
        }
    };
    if (e->config["kind"].toString()=="image") toggle("Pixel filtering","nearest");
    else {
        toggle("Show &materials","materials",true);
        toggle("Show &textures","textures",true)->setEnabled(settings["materials"].toBool(true));
        toggle("Lock &horizon","lock_horizon",true);
        toggle("Ground grid","grid",true); toggle("Axis indicator","axes"); toggle("Show edges","edges"); toggle("Orthographic camera","orthographic");
        choice("Floor / up axis","up_axis",{"Y up · XZ floor","Z up · XY floor"},{"y","z"},"y");
        choice("Lighting","lighting",{"Studio · HDRI + light kit","F3D light kit"},{"studio","lightkit"},"studio");
        auto* intensity=menu.addMenu(QString("Light intensity · %1").arg(settings["light"].toDouble(1)));
        for (double value:{0.5,1.0,1.5,2.0,3.0,5.0}) {
            auto* action=intensity->addAction(QString::number(value)); action->setCheckable(true); action->setChecked(settings["light"].toDouble(1)==value);
            connect(action,&QAction::triggered,this,[patch,value] { patch({{"light",value}}); });
        }
        if (e->config["kind"].toString()=="material") choice("Sample shape","shape",{"Sphere","Cube","Plane"},{"sphere","cube","plane"},"sphere");
    }
    choice("Background","background",{"Dark","Light","Checker"},{"dark","light","checker"},e->config["kind"].toString()=="image"?"checker":"dark");
    menu.addSeparator();
    connect(menu.addAction("Remove preview"),&QAction::triggered,this,[this,id] { service->request({{"method","remove"},{"id",id}}); });
    menu.exec(e->optionsButton->mapToGlobal(QPoint(0,e->optionsButton->height())));
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
