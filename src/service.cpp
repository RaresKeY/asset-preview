#include "service.h"
#include <QProcessEnvironment>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLocalSocket>
#include <QLocale>
#include <QMimeData>
#include <QMenu>
#include <cmath>
#include <QMenuBar>
#include <QActionGroup>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStyle>
#include <QToolBar>
#include <QInputDialog>
#include <QResizeEvent>
#include <QShowEvent>
#include <QToolButton>
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
bool videoPath(const QString& path) {
    static const QSet<QString> formats{"mp4","m4v","mov","mkv","webm","avi","ogv","mpg","mpeg","wmv","gif"};
    return formats.contains(QFileInfo(path).suffix().toLower());
}
QString absolute(const QString& path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
QByteArray readSmall(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.read(4*1024*1024) : QByteArray{}; }
QString preferenceGroup(const QString& kind) { return kind=="model" || kind=="material" ? "3d" : kind; }
QString captureDirectory() {
    QDir root(QCoreApplication::applicationDirPath()); root.cdUp();
    if (root.exists("CMakeLists.txt") && root.exists("bin/asset-preview")) return root.filePath("captures");
    const auto pictures=QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    return QDir(pictures.isEmpty()?QDir::homePath()+"/Pictures":pictures).filePath("Asset Preview");
}

class PreviewCard final : public QFrame {
public:
    PreviewCard(QWidget* parent, const QString& title, const QString& path) : QFrame(parent), fullTitle(title) {
        setFrameShape(QFrame::StyledPanel); setMinimumSize(100,100);
        content=new QVBoxLayout(this); content->setContentsMargins(0,0,0,0); content->setSpacing(0);
        header=new QFrame(this); header->setObjectName("viewOverlay");
        // Small native overlays share the container's stacking context with the
        // embedded OpenGL window. A full-window overlay would intercept orbiting.
        header->setAttribute(Qt::WA_NativeWindow);
        const QString style="QFrame#viewOverlay {background:#20262e;border:1px solid #414b57;border-radius:4px;}"
            "QFrame#viewOverlay QLabel {color:#e8edf3;background:transparent;border:none;}"
            "QToolButton {color:#e8edf3;background:transparent;border:1px solid transparent;border-radius:3px;padding:2px;}"
            "QToolButton:hover {background:#394453;} QToolButton:pressed {background:#485566;}"
            "QToolButton:focus {border-color:#82d9c8;}";
        header->setStyleSheet(style+"QFrame#viewOverlay[activePreview=true] {border-color:#667582;border-left:2px solid #667582;}");
        auto* row=new QHBoxLayout(header); row->setContentsMargins(7,1,7,1); row->setSpacing(0);
        name=new QLabel(title,header); name->setTextFormat(Qt::PlainText); name->setToolTip(title+"\n"+path);
        name->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        auto font=name->font(); font.setBold(true); name->setFont(font); row->addWidget(name,1);
        controls=new QFrame(this); controls->setObjectName("viewOverlay"); controls->setAttribute(Qt::WA_NativeWindow);
        controls->setStyleSheet(style);
        auto* buttons=new QHBoxLayout(controls); buttons->setContentsMargins(3,1,3,1); buttons->setSpacing(3);
        fit=new QToolButton(controls); fit->setText("Fit"); fit->setToolTip("Fit camera / image (also double click)");
        reset=new QToolButton(controls); reset->setText("Reset"); reset->setToolTip("Reset camera angle, pan and zoom");
        reset->setAccessibleName("Reset camera"); reset->hide();
        options=new QToolButton(controls); options->setText("⋯"); options->setToolTip("Preview options");
        options->setAccessibleName("Preview options");
        remove=new QToolButton(controls); remove->setText("×"); remove->setToolTip("Remove this preview");
        for (auto* button:{fit,reset,options,remove}) { button->setFixedSize(button==reset?50:32,28); button->setFocusPolicy(Qt::StrongFocus); buttons->addWidget(button); }
        footer=new QLabel(this); footer->setTextFormat(Qt::PlainText); footer->setAttribute(Qt::WA_NativeWindow);
        footer->setTextInteractionFlags(Qt::TextSelectableByMouse);
        footer->setStyleSheet("color:#e8edf3;background:#20262e;border:1px solid #414b57;border-radius:3px;padding:2px 5px;");
        triangles=new QLabel(this); triangles->setTextFormat(Qt::PlainText); triangles->setAttribute(Qt::WA_NativeWindow);
        triangles->setStyleSheet(footer->styleSheet()); triangles->setAccessibleName("Triangle count"); triangles->hide();
    }
    void setSurface(QWidget* surface) { content->addWidget(surface); }
    void setSelected(bool selected) {
        if (header->property("activePreview").toBool()==selected) return;
        header->setProperty("activePreview",selected);
        header->style()->unpolish(header); header->style()->polish(header); header->update();
        setAccessibleName((selected?"Active preview: ":"Preview: ")+fullTitle);
    }
    void setCompact(bool value) {
        compact=value;
        updateStatus(); place();
    }
    void setStatus(const QString& text,const QString& tooltip) {
        statusText=text; footer->setToolTip(tooltip); updateStatus();
    }
    void updateStatus() { footer->setVisible(!compact || !statusText.startsWith("Live")); place(); }
    void updateTriangles(const QJsonObject& config,const QJsonObject& metrics) {
        const auto kind=config["kind"].toString();
        const bool visible=(kind=="model" || kind=="material") && metrics["engine"].toBool() &&
            config["settings"].toObject()["triangles"].toBool(true);
        const auto count=metrics["triangles"];
        triangleText=count.isDouble()?QString("%1 tris").arg(QLocale().toString(count.toInteger())):QString("Tris —");
        triangles->setToolTip(count.isDouble()?"Loaded geometry · source triangles (including instances)":
            "Triangle count is available for GLB, glTF, OBJ and material samples.");
        triangles->setVisible(visible); place();
    }
    QJsonObject overlays(QWidget* window) const {
        QJsonObject result;
        for (const auto& item: {std::pair{"name",static_cast<QWidget*>(header)}, {"controls",static_cast<QWidget*>(controls)},
                               {"status",static_cast<QWidget*>(footer)}, {"triangles",static_cast<QWidget*>(triangles)}}) {
            const auto p=item.second->mapTo(window,QPoint());
            result[item.first]=QJsonObject{{"x",p.x()},{"y",p.y()},{"width",item.second->width()},{"height",item.second->height()},
                {"visible",item.second->isVisible()}};
        }
        return result;
    }
    void compose(QPainter& painter, QWidget* window) {
        for (auto* widget: {static_cast<QWidget*>(header),static_cast<QWidget*>(controls),static_cast<QWidget*>(footer),static_cast<QWidget*>(triangles)})
            if (widget->isVisible()) painter.drawPixmap(widget->mapTo(window,QPoint()),widget->grab());
    }
    QToolButton *fit, *reset, *options, *remove;
    bool model=false;
    QLabel* footer;
protected:
    void resizeEvent(QResizeEvent* event) override { QFrame::resizeEvent(event); place(); }
    void showEvent(QShowEvent* event) override {
        QFrame::showEvent(event); QTimer::singleShot(0,this,[this] { place(); });
    }
private:
    void place() {
        name->ensurePolished(); footer->ensurePolished(); triangles->ensurePolished(); controls->ensurePolished();
        const int inset=compact?4:6;
        const bool dense=compact || width()<300;
        fit->setVisible(!dense); remove->setVisible(!dense);
        reset->setVisible(model && !dense);
        const int headerHeight=compact?30:34;
        const int controlWidth=controls->sizeHint().width();
        controls->setGeometry(width()-inset-controlWidth,inset,controlWidth,headerHeight);
        controls->layout()->activate();
        const int available=std::max(1,width()-2*inset-controlWidth-4);
        const int nameWidth=std::min(available,name->fontMetrics().horizontalAdvance(fullTitle)+16);
        header->setGeometry(inset,inset,nameWidth,headerHeight);
        header->layout()->activate();
        const int textWidth=std::max(0,nameWidth-16);
        name->setText(name->fontMetrics().horizontalAdvance(fullTitle)<=textWidth ? fullTitle :
            name->fontMetrics().elidedText(fullTitle,Qt::ElideRight,textWidth));
        const int badgeWidth=std::max(1,width()-2*inset);
        footer->setText(footer->fontMetrics().elidedText(statusText,Qt::ElideRight,std::max(0,badgeWidth-12)));
        footer->setGeometry(inset,inset+headerHeight+4,std::min(badgeWidth,footer->sizeHint().width()),24);
        triangles->setText(triangles->fontMetrics().elidedText(triangleText,Qt::ElideRight,std::max(0,badgeWidth-12)));
        triangles->setGeometry(inset,std::max(inset,height()-inset-24),std::min(badgeWidth,triangles->sizeHint().width()),24);
        header->raise(); controls->raise(); footer->raise(); triangles->raise();
    }
    QString fullTitle, statusText, triangleText;
    QFrame *header, *controls;
    QLabel *name, *triangles;
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
        if (doc.object().contains("preferences") && !doc.object()["preferences"].isObject()) { error="Invalid saved preferences"; return false; }
        preferences=doc.object()["preferences"].toObject();
        for (auto i=preferences.begin();i!=preferences.end();++i) {
            if (!QSet<QString>{"3d","image","video"}.contains(i.key()) || !i.value().isObject()) { error="Invalid saved preference group"; return false; }
            QJsonObject config{{"path",statePath+"/preference-validation"},{"kind",i.key()=="3d"?"model":i.key()},{"settings",i.value()}};
            const auto invalid=validate(config,true);
            if (!invalid.isEmpty()) { error="Invalid saved preferences: "+invalid; return false; }
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
        if (doc.object().contains("comparison")) {
            if (!doc.object()["comparison"].isArray()) { error="Invalid saved comparison"; return false; }
            for (const auto& id:doc.object()["comparison"].toArray()) {
                const auto entry=entries.value(id.toString());
                if (!id.isString() || !entry || (entry->config["kind"]!="model" && entry->config["kind"]!="material") || comparison.contains(id.toString())) { error="Invalid saved comparison preview"; return false; }
                comparison.append(id.toString());
            }
            if (!comparison.isEmpty() && comparison.size()!=2) { error="Comparison requires two previews"; return false; }
        }
        if (doc.object().contains("camera_sync") && !doc.object()["camera_sync"].isBool()) { error="Invalid saved camera sync"; return false; }
        cameraSync=doc.object()["camera_sync"].toBool(true);
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
    auto data=QJsonDocument(QJsonObject{{"version",1},{"entries",rows},{"selected",selected},{"layout",layout},{"grid_size",gridSize},{"compact",compact},{"preferences",preferences},
        {"comparison",QJsonArray::fromStringList(comparison)},{"camera_sync",cameraSync}}).toJson();
    if (file.write(data) != data.size() || !file.commit()) throw std::runtime_error("Cannot save preview registry");
}
QString Service::validate(QJsonObject& c, bool restore) {
    if (QJsonDocument(c).toJson(QJsonDocument::Compact).size()>8192) return "Preview configuration exceeds 8 KiB";
    QString path=c["path"].toString();
    if (path.isEmpty() || !QFileInfo(path).isAbsolute()) return "An absolute output file path is required";
    if (!restore && QFileInfo(path).isDir()) return "Output must be a file";
    c["path"]=absolute(path);
    QString kind=c["kind"].toString("auto");
    if (kind == "auto") kind=videoPath(path)?"video":imagePath(path)?"image":"model";
    if (kind != "image" && kind != "model" && kind != "material" && kind != "video") return "Kind must be image, model, material or video";
    if (!restore && kind!="video" && QFileInfo(path).size() > 256*1024*1024) return "Preview file exceeds 256 MiB";
    c["kind"]=kind;
    if (c.contains("image_view")) {
        if (kind!="image" || !c["image_view"].isObject()) return "image_view requires an image preview object";
        const auto view=c["image_view"].toObject(); const double zoom=view["zoom"].toDouble(-1);
        const auto pan=view["pan"].toArray();
        if (zoom<.05 || zoom>32 || pan.size()!=2) return "Invalid image view";
        for (const auto& value:pan) if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble())>1e6) return "Invalid image pan";
    }
    if (c.contains("camera")) {
        if ((kind!="model" && kind!="material") || !c["camera"].isObject()) return "camera requires a 3D preview object";
        const auto camera=c["camera"].toObject();
        for (const auto& key:{"position","focal","up"}) {
            const auto values=camera[key].toArray();
            if (values.size()!=3) return "Camera vectors must contain three numbers";
            for (const auto& value:values) if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble())>1e12) return "Invalid camera vector";
        }
        const double angle=camera["angle"].toDouble(-1), span=camera["parallel_span"].toDouble(-1);
        if (!std::isfinite(angle) || angle<=0 || angle>=180 || !std::isfinite(span) || span<=0 || span>1e12) return "Invalid camera scale";
        double distance=0, upLength=0;
        for (int i=0;i<3;++i) {
            const double d=camera["position"].toArray()[i].toDouble()-camera["focal"].toArray()[i].toDouble();
            distance+=d*d; const double u=camera["up"].toArray()[i].toDouble(); upLength+=u*u;
        }
        if (distance<1e-18 || upLength<1e-18) return "Degenerate camera";
    }
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
    const QSet<QString> known{"grid","axes","edges","orthographic","nearest","light","background","shape","roughness","metallic","materials","textures","lock_horizon","up_axis","lighting","paused","muted","loop","triangles"};
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
    const auto oldComparison=comparison;
    const QString error=validate(config,restore);
    if (!error.isEmpty()) return failure(error);
    const QString id=config["id"].toString();
    if (!entries.contains(id) && entries.size()>=256) return failure("Preview limit reached (256)");
    auto prior=entries.value(id);
    const bool existing=bool(prior);
    QJsonObject old=prior?prior->config:QJsonObject{};
    if (!restore) {
        auto settings=preferences.value(preferenceGroup(config["kind"].toString())).toObject();
        // Reconnecting the same asset keeps its inspection choices. Explicit
        // registration options still win over remembered preferences.
        if (existing && old["kind"]==config["kind"]) {
            if (!config.contains("camera") && old.contains("camera")) config["camera"]=old["camera"];
            if (!config.contains("image_view") && old.contains("image_view")) config["image_view"]=old["image_view"];
            const auto previous=old["settings"].toObject();
            for (auto i=previous.begin();i!=previous.end();++i) settings[i.key()]=i.value();
        }
        const auto explicitSettings=config["settings"].toObject();
        for (auto i=explicitSettings.begin();i!=explicitSettings.end();++i) settings[i.key()]=i.value();
        config["settings"]=settings;
        const auto invalid=validate(config);
        if (!invalid.isEmpty()) return failure(invalid);
    }
    if (!prior) { prior=std::make_shared<Entry>(); entries[id]=prior; order.append(id); }
    prior->config=config;
    if (comparison.contains(id) && config["kind"]!="model" && config["kind"]!="material") comparison.clear();
    if (selected.isEmpty()) selected=id;
    if (!restore) {
        try { save(); }
        catch (const std::exception& e) {
            comparison=oldComparison;
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
    result["selected"]=e.config["id"].toString()==selected;
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
        const auto* reset=static_cast<PreviewCard*>(e.card)->reset;
        const auto resetPosition=reset->mapTo(window.get(),QPoint());
        auto controls=result["controls"].toObject();
        controls["reset"]=QJsonObject{{"x",resetPosition.x()},{"y",resetPosition.y()},
            {"width",reset->width()},{"height",reset->height()},{"visible",reset->isVisible()}};
        result["controls"]=controls;
        result["status_visible"]=e.statusLabel->isVisible();
        result["overlays"]=static_cast<PreviewCard*>(e.card)->overlays(window.get());
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
                 {"platform",QGuiApplication::platformName()},{"options_open",bool(QApplication::activePopupWidget())},{"preferences",preferences},
                 {"comparison",QJsonArray::fromStringList(comparison)},{"camera_sync",cameraSync},{"closed_count",closedEntries.size()}});
        }
        if (method=="defaults") {
            const auto group=r["group"].toString();
            if (!QSet<QString>{"image","video","3d"}.contains(group)) return failure("Invalid defaults group");
            if (!r["settings"].isObject()) return failure("settings must be an object");
            auto remembered=preferences.value(group).toObject();
            const auto patch=r["settings"].toObject();
            for (auto i=patch.begin();i!=patch.end();++i) remembered[i.key()]=i.value();
            QJsonObject config{{"path",statePath+"/preference-validation"},{"kind",group=="3d"?"model":group},{"settings",remembered}};
            const auto error=validate(config); if (!error.isEmpty()) return failure(error);
            const auto old=preferences; preferences[group]=remembered;
            try { save(); } catch (...) { preferences=old; throw; }
            return success();
        }
        if (method=="add") return add(r["entry"].toObject());
        if (method=="reopen") {
            if (closedEntries.isEmpty()) return failure("No closed previews to reopen");
            if (entries.size()>=256) return failure("Preview limit reached (256)");
            auto closed=closedEntries.last(); auto config=closed.config;
            const auto invalid=validate(config,true); if (!invalid.isEmpty()) return failure(invalid);
            const auto reopenId=config["id"].toString();
            if (entries.contains(reopenId)) { closedEntries.removeLast(); return request(r); }
            const auto oldOrder=order; const auto oldSelected=selected; const auto oldComparison=comparison;
            auto entry=std::make_shared<Entry>(); entry->config=config; entries[reopenId]=entry;
            order.insert(std::clamp(closed.index,0,int(order.size())),reopenId); selected=reopenId; comparison.clear();
            try { save(); } catch (...) { entries.remove(reopenId); order=oldOrder; selected=oldSelected; comparison=oldComparison; throw; }
            closedEntries.removeLast(); selectionHistory.removeAll(reopenId);
            if (!oldSelected.isEmpty()) { selectionHistory.removeAll(oldSelected); selectionHistory.append(oldSelected); }
            reconcile(); return success({{"id",reopenId}});
        }
        if (method=="compare") {
            QStringList ids=comparison;
            if (r.contains("ids")) {
                if (!r["ids"].isArray()) return failure("Comparison IDs must be an array");
                ids.clear();
                for (const auto& value:r["ids"].toArray()) {
                    const auto entry=entries.value(value.toString());
                    if (!value.isString() || !entry || (entry->config["kind"]!="model" && entry->config["kind"]!="material") || ids.contains(value.toString())) return failure("Comparison requires distinct 3D previews");
                    ids.append(value.toString());
                }
                if (!ids.isEmpty() && ids.size()!=2) return failure("Comparison requires two previews");
            }
            if (r.contains("camera_sync") && !r["camera_sync"].isBool()) return failure("camera_sync must be boolean");
            const auto oldIds=comparison; const auto oldSelected=selected; const bool oldSync=cameraSync;
            comparison=ids; cameraSync=r["camera_sync"].toBool(cameraSync);
            if (!ids.isEmpty() && !ids.contains(selected)) selected=ids.first();
            try { save(); } catch (...) { comparison=oldIds; selected=oldSelected; cameraSync=oldSync; throw; }
            reconcile(); if (cameraSync) syncCamera(selected);
            return success();
        }
        if (method=="close_all") {
            const auto previousOrder=order; const auto previousSelected=selected;
            const auto oldComparison=comparison;
            order.clear(); selected.clear(); comparison.clear();
            try { save(); } catch (...) { order=previousOrder; selected=previousSelected; comparison=oldComparison; throw; }
            for (const auto& id:previousOrder) if (id!=previousSelected) closedEntries.append({entries[id]->config,int(previousOrder.indexOf(id))});
            if (entries.contains(previousSelected)) closedEntries.append({entries[previousSelected]->config,int(previousOrder.indexOf(previousSelected))});
            while (closedEntries.size()>32) closedEntries.removeFirst();
            selectionHistory.clear();
            // Persist first so failed saves leave running previews intact.
            for (const auto& id:previousOrder) deactivate(id);
            entries.clear(); reconcile(); return success();
        }
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
            const auto oldComparison=comparison;
            layout=next; gridSize=size; compact=r["compact"].toBool(compact);
            if (r.contains("layout") || r.contains("grid_size")) comparison.clear();
            try { save(); } catch (...) { layout=previous; gridSize=previousSize; compact=previousCompact; comparison=oldComparison; throw; }
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
            if (auto* popup=QApplication::activePopupWidget())
                painter.drawPixmap(popup->mapToGlobal(QPoint())-window->mapToGlobal(QPoint()),popup->grab());
            painter.end();
            if (!capture.save(path,"PNG")) return failure("Cannot save capture");
            return success();
        }
        if (!entries.contains(id)) return failure("Unknown preview: " + id);
        auto e=entries[id];
        if (method=="remove") {
            const auto previousOrder=order; const auto previousSelected=selected;
            const auto oldComparison=comparison;
            const int index=order.indexOf(id);
            entries.remove(id); order.removeAll(id);
            if (comparison.contains(id)) comparison.clear();
            if (selected==id) {
                selected.clear();
                for (auto it=selectionHistory.crbegin();it!=selectionHistory.crend();++it)
                    if (entries.contains(*it)) { selected=*it; break; }
                if (selected.isEmpty()) selected=order.value(std::max(0,index-1));
            }
            try { save(); } catch (...) {
                entries[id]=e; order=previousOrder; selected=previousSelected; comparison=oldComparison; throw;
            }
            closedEntries.append({e->config,index}); if (closedEntries.size()>32) closedEntries.removeFirst();
            selectionHistory.removeAll(id);
            selectionHistory.removeAll(selected);
            // Keep live views intact until the removal has been persisted.
            entries[id]=e; deactivate(id); entries.remove(id);
            reconcile(); return success();
        }
        if (method=="select") {
            if (selected==id) return success();
            const auto previous=selected; selected=id;
            const auto oldComparison=comparison;
            if (!comparison.isEmpty() && !comparison.contains(id)) comparison.clear();
            try { save(); } catch (...) { selected=previous; comparison=oldComparison; throw; }
            if (previous!=id && !previous.isEmpty()) {
                selectionHistory.removeAll(previous);
                selectionHistory.append(previous);
                selectionHistory.removeAll(id);
            }
            // A visible card only needs its selection state updated.
            // Retain its existing layout and native input focus.
            if (e->active && comparison==oldComparison && window) window->navigation();
            else reconcile();
            return success();
        }
        if (method=="reload") { if (e->active) { if (!e->config["command"].toArray().isEmpty()) build(id); else reload(id); } return success(); }
        if (method=="camera_view") {
            const auto view=r["view"].toString();
            if (!e->view || (e->config["kind"]!="model" && e->config["kind"]!="material")) return failure("Camera views require a visible 3D preview");
            if (!QSet<QString>{"front","back","left","right","top","bottom","isometric"}.contains(view)) return failure("Invalid camera view");
            e->view->cameraView(view); return success();
        }
        if (method=="seek") {
            if (e->config["kind"].toString()!="video" || !e->view) return failure("Seek requires a visible video");
            if (!r["seconds"].isDouble() || r["seconds"].toDouble()<0) return failure("Seek requires nonnegative seconds");
            QString error; return e->view->seek(r["seconds"].toDouble(),error)?success():failure(error);
        }
        if (method=="settings") {
            auto config=e->config, settings=config["settings"].toObject();
            const auto patch=r["settings"].toObject();
            if (!r["settings"].isObject()) return failure("settings must be an object");
            for (auto i=patch.begin();i!=patch.end();++i) settings[i.key()]=i.value();
            config["settings"]=settings; QString error=validate(config);
            if (!error.isEmpty()) return failure(error);
            const auto old=e->config; e->config=config;
            const auto previousPreferences=preferences;
            const auto group=preferenceGroup(config["kind"].toString());
            auto remembered=preferences.value(group).toObject();
            for (auto i=patch.begin();i!=patch.end();++i) remembered[i.key()]=i.value();
            preferences[group]=remembered;
            try { save(); } catch (...) { e->config=old; preferences=previousPreferences; throw; }
            if (e->view) e->view->settings(config);
            if (e->card) status(*e,e->status);
            return success();
        }
        return failure("Unknown method");
    } catch (const std::exception& e) { return failure(QString::fromUtf8(e.what())); }
}
void Service::status(Entry& e, const QString& text) {
    e.status=text;
    if (e.card) {
        auto* card=static_cast<PreviewCard*>(e.card);
        card->setStatus(text,text + (e.log.isEmpty()?"":"\n\n"+e.log));
        card->updateTriangles(e.config,e.view?e.view->metrics():QJsonObject{});
        if (e.config["kind"].toString()=="video") {
            const bool paused=e.config["settings"].toObject()["paused"].toBool();
            card->fit->setText(paused?"▶":"Ⅱ"); card->fit->setToolTip(paused?"Play video · Space":"Pause video · Space");
        }
    }
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
    if (e->config["kind"].toString()!="video" && QFileInfo(e->config["path"].toString()).size()>256*1024*1024) { status(*e,"File exceeds 256 MiB"); return; }
    const bool building=e->process && e->process->state()!=QProcess::NotRunning;
    if (e->view->reload(e->config,error)) {
        if (e->view->loading()) { status(*e,building?"Building · loading video":"Loading · video"); return; }
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
    // Bundled Qt/Python/library paths belong to the viewer, not its generators.
    // Restore the caller's environment for project wrappers and host runtimes.
    auto generatorEnv=QProcessEnvironment::systemEnvironment();
    if (generatorEnv.contains("ASSET_PREVIEW_BUNDLE_ROOT")) {
        for (const auto* key:{"LD_LIBRARY_PATH","QT_PLUGIN_PATH","PYTHONHOME","PYTHONNOUSERSITE"}) {
            const QString original=QStringLiteral("ASSET_PREVIEW_ORIGINAL_")+QString::fromLatin1(key);
            const QString value=generatorEnv.value(original);
            if (value.isEmpty()) generatorEnv.remove(QString::fromLatin1(key));
            else generatorEnv.insert(QString::fromLatin1(key),value);
            generatorEnv.remove(original);
        }
        generatorEnv.remove("ASSET_PREVIEW_BUNDLE_ROOT");
        p->setProcessEnvironment(generatorEnv);
    }
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
    if (qEnvironmentVariableIsSet("ASSET_PREVIEW_FLATPAK")) {
        // Watch-bus sends SIGINT, which shell background jobs may ignore.
        // A host supervisor retires its separately owned child group instead.
        QFile helper(QCoreApplication::applicationDirPath()+"/../flatpak/host-generator.py");
        if (!helper.open(QIODevice::ReadOnly)) { status(*e,"Generator failed · missing host supervisor"); return; }
        arguments.prepend(args[0].toString());
        arguments.prepend(QString::fromUtf8(helper.readAll()));
        arguments.prepend("-c"); arguments.prepend("python3");
        arguments.prepend(QStringLiteral("--directory=")+e->config["cwd"].toString());
        arguments.prepend("--watch-bus"); arguments.prepend("--host");
        p->start("/usr/bin/flatpak-spawn",arguments);
    } else p->start(args[0].toString(),arguments);
}
void Service::syncCamera(const QString& id) {
    if (!cameraSync || comparison.size()!=2 || !comparison.contains(id) || syncingCamera) return;
    const auto source=entries.value(id), target=entries.value(comparison[id==comparison[0]?1:0]);
    if (!source || !target || !source->view || !target->view) return;
    auto camera=source->view->cameraState(); const auto home=source->view->cameraHome(), targetHome=target->view->cameraHome();
    if (camera.isEmpty() || home.isEmpty() || targetHome.isEmpty() || target->view->cameraState().isEmpty()) return;
    syncingCamera=true;
    const auto settings=source->config["settings"].toObject(); auto targetSettings=target->config["settings"].toObject();
    for (const auto& key:{"orthographic","up_axis","lock_horizon"}) if (settings.contains(key)) targetSettings[key]=settings[key]; else targetSettings.remove(key);
    if (targetSettings!=target->config["settings"].toObject()) { target->config["settings"]=targetSettings; target->view->settings(target->config); }
    auto radius=[](const QJsonObject& c) { double value=0; const auto p=c["position"].toArray(), f=c["focal"].toArray();
        for (int i=0;i<3;++i) value+=std::pow(p[i].toDouble()-f[i].toDouble(),2);
        return std::sqrt(value); };
    const double ratio=radius(targetHome)/std::max(1e-9,radius(home));
    const auto center=home["focal"].toArray(), targetCenter=targetHome["focal"].toArray();
    for (const auto& key:{"position","focal"}) {
        const auto values=camera[key].toArray(); QJsonArray moved;
        for (int i=0;i<3;++i) moved.append(targetCenter[i].toDouble()+(values[i].toDouble()-center[i].toDouble())*ratio);
        camera[key]=moved;
    }
    camera["parallel_span"]=camera["parallel_span"].toDouble()*ratio;
    target->config["camera"]=camera; target->view->setCameraState(camera);
    syncingCamera=false;
}
void Service::inspectionChanged(const QString& id,const QJsonObject& state,bool persist) {
    const auto entry=entries.value(id); if (!entry || !entry->active || state.isEmpty()) return;
    const auto key=entry->config["kind"].toString()=="image"?"image_view":"camera";
    entry->config[key]=state;
    if (key==QString("camera")) syncCamera(id);
    if (persist) {
        try { save(); }
        catch (const std::exception& error) { if (window) window->reportError(QString::fromUtf8(error.what())); }
    }
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
        if (!comparison.isEmpty()) visible=comparison;
        else { int i=std::max(0,int(order.indexOf(selected)));
            const int count=pageSize(); visible=order.mid((i/count)*count,count); }
    }
    for (const auto& id:order) if (!visible.contains(id)) deactivate(id);
    for (const auto& id:visible) activate(id);
    if (window) { window->arrange(visible); window->navigation(); }
    watchers();
#if defined(__GLIBC__)
    if (wasActive && visible.isEmpty()) ::malloc_trim(0);
#endif
}
QJsonObject Service::navigate(int delta) {
    if (!comparison.isEmpty()) return success();
    if (order.isEmpty()) return success();
    int index=std::max(0,int(order.indexOf(selected)));
    if (layout=="grid") {
        const int page=index/pageSize();
        const int target=std::clamp(page+delta,0,int(order.size()-1)/pageSize());
        if (target==page) return success();
        index=target*pageSize();
    } else {
        const int target=std::clamp(index+delta,0,int(order.size())-1);
        if (target==index) return success();
        index=target;
    }
    return request({{"method","select"},{"id",order[index]}});
}

void PreviewWindow::setupMenus(QToolButton* openButton) {
    menuBar()->setNativeMenuBar(false);
    auto invoke=[this](QJsonObject command) {
        const auto result=service->request(command);
        if (!result["ok"].toBool()) reportError(result["error"].toString());
    };
    auto command=[this](QJsonObject request) {
        canvas->setFocus(Qt::OtherFocusReason);
        const auto result=service->request(request);
        activateWindow();
        if (!result["ok"].toBool()) reportError(result["error"].toString());
        const auto entry=service->entries.value(service->selected);
        if (entry && entry->surface) entry->surface->setFocus(Qt::OtherFocusReason);
    };
    auto action=[this](QMenu* menu,const QString& label,const QString& icon,const QKeySequence& shortcut,const std::function<void()>& run) {
        auto* item=menu->addAction(QIcon::fromTheme(icon),label); item->setShortcut(shortcut); item->setAutoRepeat(false);
        addAction(item); connect(item,&QAction::triggered,this,run); return item;
    };
    auto* file=menuBar()->addMenu("&File");
    action(file,"&Open Assets…","document-open",QKeySequence::Open,[openButton] { openButton->click(); });
    file->addSeparator();
    saveAction=action(file,"Save &Screenshot","document-save",QKeySequence("Ctrl+Shift+S"),[this] { saveScreenshot(service->selected); });
    copyAction=action(file,"&Copy Screenshot","edit-copy",QKeySequence("Ctrl+Shift+C"),[this] { saveScreenshot(service->selected,true); });
    action(file,"Save Window Screenshot","document-save-as",{},[this] { saveScreenshot(service->selected,false,true); });
    action(file,"Open Screenshot Folder","folder-pictures",{},[this] {
        const auto directory=captureDirectory();
        if (!QDir().mkpath(directory)) { reportError("Cannot create screenshot folder: "+directory); return; }
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory))) reportError("Cannot open screenshot folder: "+directory);
    });
    file->addSeparator();
    closeAction=action(file,"&Close Preview","window-close",QKeySequence::Close,[this,command] { command({{"method","remove"},{"id",service->selected}}); });
    reopenAction=action(file,"&Reopen Closed Preview","edit-undo",QKeySequence("Ctrl+Shift+T"),[command] { command({{"method","reopen"}}); });
    auto* closeAllAction=action(file,"Close &All Previews","window-close",{},[this] { closeAllButton->click(); });
    connect(file,&QMenu::aboutToShow,this,[this,closeAllAction] { closeAllAction->setEnabled(!service->order.isEmpty()); });
    file->addSeparator();
    action(file,"&Quit","application-exit",QKeySequence::Quit,[invoke] { invoke({{"method","quit"}}); });

    auto* view=menuBar()->addMenu("&View");
    fitAction=action(view,"&Fit View","zoom-fit-best",QKeySequence("F"),[this] {
        const auto entry=service->entries.value(service->selected); if (entry && entry->view) entry->view->fit();
    });
    modelActions.append(action(view,"&Reset Camera","view-refresh",QKeySequence("Ctrl+Home"),[this] {
        const auto entry=service->entries.value(service->selected); if (entry && entry->view) entry->view->resetView();
    }));
    auto* camera=view->addMenu("Camera &Views");
    const QStringList names{"Front","Back","Right","Left","Top","Bottom","Isometric"};
    const QStringList values{"front","back","right","left","top","bottom","isometric"};
    const QStringList keys{"1","Ctrl+1","3","Ctrl+3","7","Ctrl+7","9"};
    for (int i=0;i<names.size();++i) {
        auto* item=action(camera,names[i],"",QKeySequence(keys[i]),[this,invoke,value=values[i]] { invoke({{"method","camera_view"},{"id",service->selected},{"view",value}}); });
        const int digit=keys[i].right(1).toInt(); const bool ctrl=keys[i].startsWith("Ctrl");
        item->setShortcuts({QKeySequence(keys[i]),QKeySequence(int(Qt::KeypadModifier)|(ctrl?int(Qt::ControlModifier):0)|int(Qt::Key_0+digit))});
        modelActions.append(item);
    }
    auto* current=view->addMenu("Preview &Options");
    connect(current,&QMenu::aboutToShow,this,[this,current] { current->clear(); populatePreviewMenu(current,service->selected,false); });
    view->addSeparator();
    auto* layoutMenu=view->addMenu("&Layout");
    auto* layoutGroup=new QActionGroup(layoutMenu); layoutGroup->setExclusive(true);
    for (int i=0;i<4;++i) {
        auto* item=layoutMenu->addAction(i==0?"Single Preview":QString("%1 × %1 Grid").arg(i+1));
        item->setCheckable(true); item->setData(i); layoutGroup->addAction(item);
        connect(item,&QAction::triggered,this,[invoke,i] {
            QJsonObject patch{{"method","layout"},{"layout",i==0?"single":"grid"}};
            if (i>0) patch["grid_size"]=i+1;
            invoke(patch);
        });
    }
    connect(layoutMenu,&QMenu::aboutToShow,this,[this,layoutGroup] {
        const int index=service->layout=="single"?0:service->gridSize-1;
        for (auto* item:layoutGroup->actions()) item->setChecked(service->comparison.isEmpty() && item->data().toInt()==index);
    });
    auto* compact=view->addAction("&Compact Mode"); compact->setCheckable(true);
    connect(compact,&QAction::triggered,this,[invoke](bool checked) { invoke({{"method","layout"},{"compact",checked}}); });
    connect(view,&QMenu::aboutToShow,this,[this,current,compact] { current->setEnabled(!service->selected.isEmpty()); compact->setChecked(service->compact); });
    auto* compare=view->addMenu("&Compare With");
    connect(compare,&QMenu::aboutToShow,this,[this,compare,invoke] {
        compare->clear(); const auto source=service->entries.value(service->selected);
        if (!source || (source->config["kind"]!="model" && source->config["kind"]!="material")) { compare->addAction("Select a 3D preview first")->setEnabled(false); return; }
        for (const auto& id:service->order) {
            const auto entry=service->entries[id]; if (id==service->selected || (entry->config["kind"]!="model" && entry->config["kind"]!="material")) continue;
            auto* item=compare->addAction(entry->config["label"].toString());
            connect(item,&QAction::triggered,this,[this,id,invoke] { invoke({{"method","compare"},{"ids",QJsonArray{service->selected,id}},{"camera_sync",true}}); });
        }
        if (compare->isEmpty()) compare->addAction("Open another 3D preview to compare")->setEnabled(false);
    });
    auto* sync=view->addAction("&Sync Comparison Cameras"); sync->setCheckable(true);
    connect(sync,&QAction::triggered,this,[invoke](bool enabled) { invoke({{"method","compare"},{"camera_sync",enabled}}); });
    auto* end=action(view,"&End Comparison","",{},[invoke] { invoke({{"method","compare"},{"ids",QJsonArray{}}}); });
    connect(view,&QMenu::aboutToShow,this,[this,sync,end] { sync->setEnabled(!service->comparison.isEmpty()); sync->setChecked(service->cameraSync); end->setEnabled(!service->comparison.isEmpty()); });

    auto* go=menuBar()->addMenu("&Go");
    auto* previousAction=go->addAction(QIcon::fromTheme("go-previous"),"&Previous\tLeft");
    auto* nextAction=go->addAction(QIcon::fromTheme("go-next"),"&Next\tRight");
    connect(previousAction,&QAction::triggered,previous,&QToolButton::click); connect(nextAction,&QAction::triggered,next,&QToolButton::click);
    connect(go,&QMenu::aboutToShow,this,[this,previousAction,nextAction] { previousAction->setEnabled(previous->isEnabled()); nextAction->setEnabled(next->isEnabled()); });
    auto* reloadAction=action(go,"&Reload / Rebuild","view-refresh",QKeySequence("Ctrl+R"),[this,invoke] { if (!service->selected.isEmpty()) invoke({{"method","reload"},{"id",service->selected}}); });
    connect(go,&QMenu::aboutToShow,this,[this,reloadAction] { reloadAction->setEnabled(!service->selected.isEmpty()); });

    auto* settings=menuBar()->addMenu("&Settings");
    connect(settings,&QMenu::aboutToShow,this,[this,settings] {
        settings->clear(); settings->addSection("Defaults for New Previews");
        for (const auto& group:QStringList{"image","video","3d"}) {
            auto* defaults=settings->addMenu(group=="3d"?"&3D / Materials":group=="image"?"&Images":"&Videos");
            populateSettings(defaults,group=="3d"?"material":group,service->preferences.value(group).toObject(),[this,group](QJsonObject patch) {
                const auto result=service->request({{"method","defaults"},{"group",group},{"settings",patch}});
                if (!result["ok"].toBool()) reportError(result["error"].toString());
            });
        }
    });
    auto* help=menuBar()->addMenu("&Help");
    action(help,"&Keyboard Shortcuts","help-contents",QKeySequence::HelpContents,[this] {
        QMessageBox::information(this,"Keyboard Shortcuts","Open assets: Ctrl+O\nClose preview: Ctrl+W\nReopen closed preview: Ctrl+Shift+T\nPrevious / next: Left / Right\nFit view: F\nReset camera: Ctrl+Home\nEdges: E · Triangle count: M (3D)\nCamera: 1 front · 3 right · 7 top · 9 isometric\nCtrl+1 / 3 / 7: opposite view · Numpad supported\nSave screenshot: Ctrl+Shift+S\nCopy screenshot: Ctrl+Shift+C\nReload / rebuild: Ctrl+R\nVideo: Space pause · M mute · Home restart\nVideo seek: Shift+Left / Right");
    });
    action(help,"&About Asset Preview","help-about",{},[this] { QMessageBox::about(this,"About Asset Preview","Asset Preview\nLive images, models, videos and baked materials.\nMIT licensed. Dependencies retain their own licenses."); });
}

PreviewWindow::PreviewWindow(Service* s) : service(s) {
    setWindowTitle("Asset Preview"); setWindowIcon(QIcon(QStringLiteral(":/icon.svg")));
    setMinimumSize(540,380); resize(1000,720); setAcceptDrops(true);
    auto* central=new QWidget; auto* vertical=new QVBoxLayout(central); vertical->setContentsMargins(8,6,8,6); vertical->setSpacing(6);
    auto* addButton=new QToolButton; addButton->setText("Open…"); addButton->setMinimumHeight(32);
    addButton->setIcon(QIcon::fromTheme("document-open",style()->standardIcon(QStyle::SP_DialogOpenButton)));
    addButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); addButton->setAutoRaise(true);
    closeAllButton=new QToolButton; closeAllButton->setText("Close All"); closeAllButton->setMinimumHeight(32); closeAllButton->setAutoRaise(true);
    closeAllButton->setObjectName("closeAll"); closeAllButton->setToolTip("Close all previews and stop their generators; keep files and remembered settings");
    auto* controls=new QHBoxLayout;
    previous=new QToolButton; next=new QToolButton; previous->setFixedSize(32,32); next->setFixedSize(32,32);
    previous->setIcon(QIcon::fromTheme("go-previous",style()->standardIcon(QStyle::SP_ArrowLeft)));
    next->setIcon(QIcon::fromTheme("go-next",style()->standardIcon(QStyle::SP_ArrowRight)));
    previous->setAutoRaise(true); next->setAutoRaise(true);
    previous->setAccessibleName("Previous preview or page"); next->setAccessibleName("Next preview or page");
    previous->setToolTip("Previous preview / page (Left)"); next->setToolTip("Next preview / page (Right)");
    selection=new QComboBox; selection->setMinimumHeight(32); selection->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    selection->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); selection->setMinimumContentsLength(3);
    density=new QComboBox; density->addItems({"Single","2×2","3×3","4×4"}); density->setMinimumHeight(32);
    density->setToolTip("Visible previews per page: 1, 4, 9 or 16");
    compactButton=new QCheckBox("Compact"); compactButton->setToolTip("Tight spacing; keep names and options inside views; show only busy/error status");
    syncButton=new QCheckBox("Sync camera"); syncButton->setToolTip("Link orbit, pan and zoom in both comparison views"); syncButton->hide();
    position=new QLabel; position->setMinimumWidth(65); position->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    controls->addWidget(previous); controls->addWidget(selection,1); controls->addWidget(next); controls->addWidget(position);
    controls->addWidget(density); controls->addWidget(compactButton); controls->addWidget(syncButton); controls->addWidget(addButton); controls->addWidget(closeAllButton);
    auto* toolbar=new QToolBar("Preview Toolbar",this); toolbar->setObjectName("previewToolbar"); toolbar->setMovable(false);
    auto* toolbarControls=new QWidget; toolbarControls->setLayout(controls); controls->setContentsMargins(4,2,4,2);
    toolbarControls->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed); toolbar->addWidget(toolbarControls); addToolBar(toolbar);
    canvas=new QWidget; canvas->setFocusPolicy(Qt::StrongFocus);
    grid=new QGridLayout(canvas); grid->setContentsMargins(0,0,0,0); grid->setSpacing(6);
    vertical->addWidget(canvas,1); setCentralWidget(central);
    statusBar()->showMessage("Drag files to add · Left / Right to switch · close to suspend");
    connect(statusBar(),&QStatusBar::messageChanged,this,[this](const QString& message) {
        if (message.isEmpty()) statusBar()->setVisible(!service->compact);
    });
    connect(addButton,&QToolButton::clicked,this,[this] {
        for (const auto& path:QFileDialog::getOpenFileNames(this,"Add asset previews",QDir::homePath(),"Assets (*.png *.jpg *.jpeg *.webp *.bmp *.svg *.glb *.gltf *.obj *.stl *.ply *.fbx *.mp4 *.m4v *.mov *.mkv *.webm *.avi *.ogv *.mpg *.mpeg *.wmv *.gif);;All files (*)"))
            service->request({{"method","add"},{"entry",QJsonObject{{"path",path}}}});
    });
    connect(closeAllButton,&QToolButton::clicked,this,[this] {
        const auto result=service->request({{"method","close_all"}});
        if (!result["ok"].toBool()) reportError(result["error"].toString());
    });
    connect(density,&QComboBox::activated,this,[this](int index) {
        QJsonObject request{{"method","layout"},{"layout",index==0?"single":"grid"}};
        if (index>0) request["grid_size"]=index+1;
        service->request(request);
    });
    connect(compactButton,&QCheckBox::toggled,this,[this](bool value) { service->request({{"method","layout"},{"compact",value}}); });
    connect(syncButton,&QCheckBox::toggled,this,[this](bool value) { service->request({{"method","compare"},{"camera_sync",value}}); });
    auto navigate=[this](int delta) {
        if (!(delta<0?previous:next)->isEnabled()) return;
        // Move focus before destroying a native view so the next key still
        // reaches this window after the focused OpenGL child is unloaded.
        canvas->setFocus(Qt::OtherFocusReason);
        const auto result=service->navigate(delta);
        activateWindow();
        const auto entry=service->entries.value(service->selected);
        if (entry && entry->surface) entry->surface->setFocus(Qt::OtherFocusReason);
        if (!result["ok"].toBool()) reportError(result["error"].toString());
    };
    connect(previous,&QToolButton::clicked,this,[navigate] { navigate(-1); });
    connect(next,&QToolButton::clicked,this,[navigate] { navigate(1); });
    connect(selection,&QComboBox::activated,this,[this](int index) { service->request({{"method","select"},{"id",selection->itemData(index).toString()}}); });
    auto* left=new QShortcut(this); left->setKeys({QKeySequence("Left"),QKeySequence("Alt+Left")});
    connect(left,&QShortcut::activated,this,[navigate] { navigate(-1); });
    auto* right=new QShortcut(this); right->setKeys({QKeySequence("Right"),QKeySequence("Alt+Right")});
    connect(right,&QShortcut::activated,this,[navigate] { navigate(1); });
    for (const auto& binding : {std::pair{"E","edges"},std::pair{"M","triangles"}}) {
        auto* shortcut=new QShortcut(QKeySequence(binding.first),this);
        modelShortcuts.append(shortcut);
        shortcut->setAutoRepeat(false);
        connect(shortcut,&QShortcut::activated,this,[this,key=QString(binding.second)] {
            const auto entry=service->entries.value(service->selected);
            if (!entry) return;
            const auto kind=entry->config["kind"].toString();
            if (kind!="model" && kind!="material") return;
            const bool value=entry->config["settings"].toObject()[key].toBool(key=="triangles");
            const auto result=service->request({{"method","settings"},{"id",service->selected},
                {"settings",QJsonObject{{key,!value}}}});
            if (!result["ok"].toBool()) reportError(result["error"].toString());
        });
    }
    setupMenus(addButton);
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
    closeAllButton->setEnabled(n>0);
    closeAction->setEnabled(n>0); reopenAction->setEnabled(!service->closedEntries.isEmpty());
    const auto selected=service->entries.value(service->selected);
    const bool ready=selected && selected->active && selected->view && selected->view->metrics()["loads"].toInt()>0;
    saveAction->setEnabled(ready);
    copyAction->setEnabled(saveAction->isEnabled());
    const bool model=ready && (selected->config["kind"]=="model" || selected->config["kind"]=="material");
    fitAction->setEnabled(ready && selected->config["kind"]!="video");
    for (auto* item:modelActions) item->setEnabled(model);
    for (auto* item:modelShortcuts) item->setEnabled(model);
    for (const auto& id:service->order) if (service->entries[id]->card)
        static_cast<PreviewCard*>(service->entries[id]->card)->setSelected(id==service->selected);
    QSignalBlocker densityBlock(density), compactBlock(compactButton);
    density->setCurrentIndex(service->layout=="single"?0:service->gridSize-1);
    density->setEnabled(service->comparison.isEmpty());
    { QSignalBlocker blocker(syncButton); syncButton->setVisible(!service->comparison.isEmpty()); syncButton->setChecked(service->cameraSync); }
    if (!service->comparison.isEmpty()) { previous->setEnabled(false); next->setEnabled(false); position->setText("Compare"); return; }
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
        auto* empty=new QLabel("Watch your work take shape\n\nAdd or drop an image, video, model, or exported material.\nPreviews refresh when the files are saved.");
        empty->setProperty("empty",true); empty->setAlignment(Qt::AlignCenter); empty->setWordWrap(true);
        grid->addWidget(empty,0,0); grid->setRowStretch(0,1); grid->setColumnStretch(0,1);
    } else {
        for (int i=0;i<ids.size();++i) { const int cols=!service->comparison.isEmpty()?2:service->layout=="grid"?std::min(service->gridSize,int(ids.size())):1;
            static_cast<PreviewCard*>(service->entries[ids[i]]->card)->setCompact(service->compact);
            grid->addWidget(service->entries[ids[i]]->card,i/cols,i%cols); grid->setRowStretch(i/cols,1); grid->setColumnStretch(i%cols,1); }
    }
}
void PreviewWindow::createCard(Entry& e) {
    const QString id=e.config["id"].toString();
    auto* card=new PreviewCard(canvas,e.config["label"].toString(),e.config["path"].toString());
    card->model=e.config["kind"].toString()=="model" || e.config["kind"].toString()=="material";
    e.card=card; e.statusLabel=card->footer; e.optionsButton=card->options;
    service->status(e,e.status);
    QWidget* widget=nullptr;
    if (e.config["kind"].toString()=="image") { auto* image=new ImageView(e.card); e.view=image; widget=image; }
    else {
        const bool video=e.config["kind"].toString()=="video";
        auto loaded=[this,id,video](bool ok,QString error) {
            const auto e=service->entries.value(id); if (!e || !e->active) return;
            if (video) {
                if (ok) { ++e->revisions; e->retries=0; }
                else if (++e->retries<=3) {
                    const auto token=e->token;
                    QTimer::singleShot(200*(1<<e->retries),service,[this,id,token] {
                        const auto e=service->entries.value(id); if (e && e->active && e->token==token) service->reload(id);
                    });
                }
            }
            if (e->process && e->process->state()!=QProcess::NotRunning) { service->status(*e,e->status); return; }
            service->status(*e,ok?(video?"Live · video ready":"Live · model ready"):"Waiting · "+error);
            navigation();
            if (ok && !video && !service->comparison.isEmpty() && !service->syncingCamera) {
                service->syncCamera(service->selected);
                try { service->save(); } catch (const std::exception& error) { reportError(QString::fromUtf8(error.what())); }
            }
        };
        try {
            const auto handle=video?createVideo(e.config,e.card,std::move(loaded),[this,id](QJsonObject patch) {
                service->request({{"method","settings"},{"id",id},{"settings",patch}});
            }):createModel(e.config,e.card,std::move(loaded),[this,id](const QJsonObject& state,bool persist) { service->inspectionChanged(id,state,persist); });
            e.view=handle.view; widget=handle.surface;
        } catch (const std::exception& error) {
            e.status=QString(video?"Video backend unavailable · ":"3D backend unavailable · ")+QString::fromUtf8(error.what());
            qWarning("%s",qPrintable(e.status));
            service->status(e,e.status);
            auto* message=new QLabel("Build the preview backends with tools/build.sh, then reopen this preview.",e.card);
            message->setWordWrap(true); message->setAlignment(Qt::AlignCenter); widget=message;
        }
    }
    e.surface=widget;
    if (e.view) {
        e.view->activated=[this,id] { selectCard(id); };
        e.view->inspectionChanged=[this,id](const QJsonObject& state,bool persist) { service->inspectionChanged(id,state,persist); };
    }
    for (auto* object:card->findChildren<QWidget*>()) {
        object->setProperty("previewId",id); object->installEventFilter(this);
    }
    card->setProperty("previewId",id); card->installEventFilter(this);
    card->setSurface(widget);
    widget->setToolTip(e.config["kind"].toString()=="video"?"Space / double click to pause · Shift+Left / Right seek · Home restart · M mute":
        e.config["kind"].toString()=="image"?"Scroll to zoom · drag to pan · double click to fit":"Drag to orbit · right / middle drag to pan · scroll to zoom · double click to fit · E edges / lines · M triangle count");
    connect(card->fit,&QToolButton::clicked,this,[this,id] {
        const auto e=service->entries.value(id); if (!e || !e->view) return;
        if (e->config["kind"].toString()=="video") service->request({{"method","settings"},{"id",id},{"settings",QJsonObject{{"paused",!e->config["settings"].toObject()["paused"].toBool()}}}});
        else e->view->fit();
    });
    connect(card->options,&QToolButton::clicked,this,[this,id] { options(id); });
    connect(card->reset,&QToolButton::clicked,this,[this,id] {
        const auto entry=service->entries.value(id); if (entry && entry->view) entry->view->resetView();
    });
    connect(card->remove,&QToolButton::clicked,this,[this,id] { service->request({{"method","remove"},{"id",id}}); });
}
void PreviewWindow::removeCard(Entry& e) { if (e.card) { grid->removeWidget(e.card); delete e.card; } }
void PreviewWindow::selectCard(const QString& id) {
    if (id==service->selected || !service->entries.contains(id)) return;
    const auto result=service->request({{"method","select"},{"id",id}});
    if (!result["ok"].toBool()) reportError(result["error"].toString());
}
bool PreviewWindow::eventFilter(QObject* object,QEvent* event) {
    const auto id=object->property("previewId").toString();
    if (!id.isEmpty() && event->type()==QEvent::MouseButtonPress) selectCard(id);
    if (!id.isEmpty() && event->type()==QEvent::ContextMenu) {
        auto* context=static_cast<QContextMenuEvent*>(event); selectCard(id);
        QMenu menu(this); populatePreviewMenu(&menu,id,true); menu.exec(context->globalPos()); return true;
    }
    return QMainWindow::eventFilter(object,event);
}
void PreviewWindow::reportError(const QString& error) {
    statusBar()->setVisible(true);
    statusBar()->showMessage(error,5000);
}
void PreviewWindow::saveScreenshot(const QString& id,bool clipboard,bool wholeWindow) {
    const auto entry=service->entries.value(id);
    if (!wholeWindow && (!entry || !entry->view || !entry->active)) { reportError("Open a preview before taking a screenshot."); return; }
    QImage capture;
    if (wholeWindow) {
        capture=grab().toImage(); QPainter painter(&capture);
        for (const auto& item:service->entries) if (item->active && item->view && item->config["kind"]!="image")
            painter.drawImage(QRect(item->surface->mapTo(this,QPoint()),item->surface->size()),item->view->snapshot());
        for (const auto& item:service->entries) if (item->active && item->card) static_cast<PreviewCard*>(item->card)->compose(painter,this);
    } else capture=entry->view->snapshot();
    if (capture.isNull()) { reportError("This preview is not ready for a screenshot."); return; }
    if (clipboard) { QGuiApplication::clipboard()->setImage(capture); statusBar()->showMessage("Screenshot copied to clipboard",4000); return; }
    const auto directory=captureDirectory();
    if (!QDir().mkpath(directory)) { reportError("Cannot create screenshot folder: "+directory); return; }
    const auto filename=QString("%1-%2-%3.png").arg(wholeWindow?"window":id,QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"),QUuid::createUuid().toString(QUuid::Id128).left(6));
    QFile file(QDir(directory).filePath(filename));
    if (!file.open(QIODevice::WriteOnly|QIODevice::NewOnly)) { reportError(file.errorString()); return; }
    if (!capture.save(&file,"PNG")) { file.remove(); reportError("Cannot save screenshot."); return; }
    statusBar()->showMessage("Screenshot saved: "+file.fileName(),7000);
}
void PreviewWindow::cameraViews(QMenu* menu,const QString& id) {
    const QList<std::pair<QString,QString>> views{{"Front","front"},{"Back","back"},{"Right","right"},{"Left","left"},{"Top","top"},{"Bottom","bottom"},{"Isometric","isometric"}};
    const QStringList keys{"1","Ctrl+1","3","Ctrl+3","7","Ctrl+7","9"};
    for (int i=0;i<views.size();++i) {
        auto* action=menu->addAction(views[i].first+"\t"+keys[i]);
        connect(action,&QAction::triggered,this,[this,id,view=views[i].second] {
            const auto result=service->request({{"method","camera_view"},{"id",id},{"view",view}});
            if (!result["ok"].toBool()) reportError(result["error"].toString());
        });
    }
}
void PreviewWindow::populatePreviewMenu(QMenu* menu,const QString& id,bool context) {
    const auto entry=service->entries.value(id); if (!entry) return;
    const auto kind=entry->config["kind"].toString();
    auto action=[&](const QString& text,const QString& icon,const std::function<void()>& fn) {
        auto* result=menu->addAction(QIcon::fromTheme(icon),text); connect(result,&QAction::triggered,this,fn); return result;
    };
    if (kind!="video") action("&Fit View\tF","zoom-fit-best",[entry] { if (entry->view) entry->view->fit(); });
    if (kind=="model" || kind=="material") {
        action("&Reset Camera\tCtrl+Home","view-refresh",[entry] { if (entry->view) entry->view->resetView(); });
        cameraViews(menu->addMenu("Camera &Views"),id);
    }
    menu->addSeparator();
    auto patch=[this,id](QJsonObject fields) {
        const auto result=service->request({{"method","settings"},{"id",id},{"settings",fields}});
        if (!result["ok"].toBool()) reportError(result["error"].toString());
    };
    populateSettings(menu,kind,entry->config["settings"].toObject(),patch);
    if (kind=="video") {
        menu->addSeparator();
        action("Restart\tHome","media-skip-backward",[this,id] { service->request({{"method","seek"},{"id",id},{"seconds",0}}); });
        for (int delta:{-5,5}) action(delta<0?"Back 5 Seconds\tShift+Left":"Forward 5 Seconds\tShift+Right","media-seek-forward",[this,id,delta] {
            const auto entry=service->entries.value(id); if (entry && entry->view)
                service->request({{"method","seek"},{"id",id},{"seconds",std::max(0.0,entry->view->metrics()["position"].toDouble()+delta)}});
        });
    }
    if (context) {
        menu->addSeparator();
        action("Save &Screenshot\tCtrl+Shift+S","document-save",[this,id] { saveScreenshot(id); });
        action("Copy Screenshot\tCtrl+Shift+C","edit-copy",[this,id] { saveScreenshot(id,true); });
        action("&Reload / Rebuild\tCtrl+R","view-refresh",[this,id] { service->request({{"method","reload"},{"id",id}}); });
        menu->addSeparator();
        action("&Close Preview\tCtrl+W","window-close",[this,id] { service->request({{"method","remove"},{"id",id}}); });
    }
}
void PreviewWindow::options(const QString& id) {
    const auto entry=service->entries.value(id); if (!entry) return;
    selectCard(id);
    QMenu menu(this); populatePreviewMenu(&menu,id,true);
    menu.exec(entry->optionsButton->mapToGlobal(QPoint(0,entry->optionsButton->height())));
}
void PreviewWindow::populateSettings(QMenu* menu,const QString& kind,const QJsonObject& settings,
    const std::function<void(QJsonObject)>& patch) {
    auto number=[&](const QString& label,const QString& key,double fallback,double maximum) {
        const auto value=settings[key].toDouble(fallback);
        auto* action=menu->addAction(QString("%1 · %2…").arg(label).arg(value));
        connect(action,&QAction::triggered,this,[this,patch,key,label,value,maximum] {
            bool accepted=false;
            const double next=QInputDialog::getDouble(this,"Preview settings",label,value,0,maximum,3,&accepted);
            if (accepted) patch({{key,next}});
        });
    };
    auto toggle=[&](const QString& label,const QString& key,bool defaultValue=false) {
        auto* action=menu->addAction(label); action->setCheckable(true); action->setChecked(settings[key].toBool(defaultValue));
        connect(action,&QAction::toggled,this,[patch,key](bool value) { patch({{key,value}}); }); return action;
    };
    auto choice=[&](const QString& label,const QString& key,const QStringList& labels,const QStringList& values,const QString& fallback) {
        auto* sub=menu->addMenu(label);
        for (int i=0;i<values.size();++i) {
            auto* action=sub->addAction(labels[i]); action->setCheckable(true); action->setChecked(settings[key].toString(fallback)==values[i]);
            connect(action,&QAction::triggered,this,[patch,key,value=values[i]] { patch({{key,value}}); });
        }
    };
    if (kind=="image") toggle("&Pixel Filtering","nearest");
    else if (kind=="video") {
        toggle("&Paused\tSpace","paused"); toggle("&Muted\tM","muted",true); toggle("&Loop","loop",true);

    }
    else {
        menu->addSection("Display");
        toggle("Show Triangle &Count\tM","triangles",true);
        toggle("Show &Materials","materials",true);
        toggle("Show &Textures","textures",true)->setEnabled(settings["materials"].toBool(true));
        toggle("Lock &horizon","lock_horizon",true);
        toggle("Ground Grid","grid",true); toggle("Axis Indicator","axes"); toggle("Show Edges / Lines\tE","edges"); toggle("Orthographic Camera","orthographic");
        choice("Floor / up axis","up_axis",{"Y up · XZ floor","Z up · XY floor"},{"y","z"},"y");
        choice("Lighting","lighting",{"Studio · HDRI + light kit","F3D light kit"},{"studio","lightkit"},"studio");
        number("Light intensity","light",1,5);
        if (kind=="material") {
            choice("Sample shape","shape",{"Sphere","Cube","Plane"},{"sphere","cube","plane"},"sphere");
            number("Roughness (without ORM map)","roughness",0.5,1);
            number("Metallic (without ORM map)","metallic",0,1);
        }
    }
    menu->addSeparator();
    choice("Background","background",kind=="image"?QStringList{"Dark","Light","Checker"}:QStringList{"Dark","Light"},kind=="image"?QStringList{"dark","light","checker"}:QStringList{"dark","light"},kind=="image"?"checker":"dark");
}
void PreviewWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    if (!selection) return;
    closeAllButton->setVisible(width()>=780);
    position->setVisible(width()>=700);
    compactButton->setVisible(width()>=640);
    syncButton->setText(width()<760?"Sync":"Sync camera");
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
