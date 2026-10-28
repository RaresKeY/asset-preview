#include "animation_time.h"
#include "crash_log.h"
#include "codex_panel.h"
#include "reference_editor.h"
#include <QDockWidget>
#include "service.h"
#include "loading_label.h"
#include "material_graph.h"
#include "model_frame.h"
#include "inspection/paths.h"
#include "video_controls.h"
#include "fullscreen_notice.h"
#include "animation_controls.h"
#include "overview_grid.h"
#include "project_grid.h"
#include "thumbnail_job.h"
#include "project_identity.h"
#include <QSlider>
#include "theme.h"
#include <QImageReader>
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
#include <QCursor>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QWindow>
#include <QWheelEvent>
#include <QDateTime>
#include <QElapsedTimer>
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
#include <QScreen>
#include <QScopeGuard>
#include "window_placement.h"
#include <QShortcut>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStandardItemModel>
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
QJsonObject runtimeTiming;
QJsonObject success(QJsonObject data = {}) { data.insert("ok", true); return data; }
QJsonObject failure(const QString& error) { return {{"ok", false}, {"error", error}}; }
QString compactTriangleCount(qint64 count) {
    const QLocale locale;
    constexpr char suffixes[]={'\0','K','M','B'};
    double value=double(count);
    int unit=0;
    while (value>=1000. && unit<3) { value/=1000.; ++unit; }
    if (!unit) return locale.toString(count);
    value=std::round(value*10.)/10.;
    if (value>=1000. && unit<3) { value/=1000.; ++unit; }
    return locale.toString(value,'f',value==std::floor(value)?0:1)+QLatin1Char(suffixes[unit]);
}
QString stamp(const QString& path) {
    struct stat s{};
    if (::stat(QFile::encodeName(path).constData(), &s)) return "missing";
    return QString("%1:%2:%3:%4:%5:%6:%7:%8").arg(s.st_dev).arg(s.st_ino).arg(s.st_size)
        .arg(s.st_mtim.tv_sec).arg(s.st_mtim.tv_nsec).arg(s.st_ctim.tv_sec).arg(s.st_ctim.tv_nsec).arg(s.st_mode);
}
bool imagePath(const QString& path) {
    static const QSet<QString> formats{"png","jpg","jpeg","webp","bmp","tif","tiff","svg","exr","hdr"};
    return formats.contains(QFileInfo(path).suffix().toLower());
}
bool videoPath(const QString& path) {
    static const QSet<QString> formats{"mp4","m4v","mov","mkv","webm","avi","ogv","mpg","mpeg","wmv","gif"};
    return formats.contains(QFileInfo(path).suffix().toLower());
}
bool audioPath(const QString& path) {
    static const QSet<QString> formats{"wav","wave","mp3","flac","ogg","oga","opus","m4a","aac","aiff","aif","wma","ape"};
    return formats.contains(QFileInfo(path).suffix().toLower());
}
bool playbackKind(const QString& kind) { return kind=="video" || kind=="audio"; }
QJsonArray droppedFiles(const QMimeData* mime) {
    QJsonArray paths;
    for (const auto& url:mime->urls())
        if (url.isLocalFile() && !QFileInfo(url.toLocalFile()).isDir()) paths.append(url.toLocalFile());
    return paths;
}
bool acceptFileDrag(QDropEvent* event) {
    if (!(event->possibleActions() & Qt::CopyAction) || droppedFiles(event->mimeData()).isEmpty()) {
        event->ignore(); return false;
    }
    event->setDropAction(Qt::CopyAction); event->accept(); return true;
}
QString absolute(const QString& path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
QString preferenceGroup(const QString& kind) { return kind=="model" || kind=="material" ? "3d" : kind; }
QString captureDirectory() { return screenshotDirectory(); }
QJsonObject inspectionConfig(const Entry& entry) {
    auto config=entry.config, view=entry.inspectionView, options=config.value("inspection").toObject();
    if (config.value("camera").isObject()) view["camera"]=config["camera"];
    view["orthographic"]=config["settings"].toObject()["orthographic"].toBool();
    options["view"]=view; config["inspection"]=options;
    return config;
}

class PreviewCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;
protected:
    bool hitButton(const QPoint& point) const override { return rect().contains(point); }
    void paintEvent(QPaintEvent*) override {
        // Card overlays stay dark even when the application uses a light theme.
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const QRectF box((width()-16)/2.0,(height()-16)/2.0,16,16);
        painter.setBrush(QColor(underMouse()?"#394453":"#171c22"));
        painter.setPen(QPen(QColor(hasFocus()?"#82d9c8":"#87939f"),1));
        painter.drawRect(box);
        if (isChecked()) {
            painter.setPen(QPen(QColor("#e8edf3"),2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
            painter.drawPolyline(QPolygonF{box.topLeft()+QPointF(3,8),box.topLeft()+QPointF(7,12),box.topLeft()+QPointF(13,4)});
        }
    }
};

class PreviewCard final : public QFrame {
public:
    PreviewCard(QWidget* parent, const QString& title, const QString& path, const QString& kind,bool native)
        : QFrame(parent), fullTitle(title), video(kind=="video") {
        setFrameShape(QFrame::StyledPanel); setMinimumSize(100,100);
        content=new QVBoxLayout(this); content->setContentsMargins(0,0,0,0); content->setSpacing(0);
        header=new QFrame(this); header->setObjectName("viewOverlay");
        // Small native overlays share the container's stacking context with the
        // embedded OpenGL window. A full-window overlay would intercept orbiting.
        header->setAttribute(Qt::WA_NativeWindow,native);
        const QString style="QFrame#viewOverlay {background:#20262e;border:1px solid #414b57;border-radius:4px;}"
            "QFrame#viewOverlay QLabel {color:#e8edf3;background:transparent;border:none;}"
            "QToolButton {color:#e8edf3;background:transparent;border:1px solid transparent;border-radius:3px;padding:2px;}"
            "QToolButton:hover {background:#394453;} QToolButton:pressed {background:#485566;}"
            "QToolButton:focus {border-color:#82d9c8;}"
            "QToolButton:checked {background:#35454b;border-color:#658c88;}";
        header->setStyleSheet(style);
        auto* row=new QHBoxLayout(header); row->setContentsMargins(7,1,7,1); row->setSpacing(0);
        name=new QLabel(title,header); name->setTextFormat(Qt::PlainText); name->setToolTip(title+"\n"+path);
        name->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        auto font=name->font(); font.setBold(true); name->setFont(font); row->addWidget(name,1);
        controls=new QFrame(this); controls->setObjectName("viewOverlay"); controls->setAttribute(Qt::WA_NativeWindow,native);
        controls->setStyleSheet(style);
        auto* buttons=new QHBoxLayout(controls); buttons->setContentsMargins(3,1,3,1); buttons->setSpacing(3);
        fit=new QToolButton(controls); fit->setText("Fit"); fit->setToolTip("Fit camera / image (also double click)");
        reset=new QToolButton(controls); reset->setText("Reset"); reset->setToolTip("Reset camera angle, pan and zoom");
        reset->setAccessibleName("Reset camera"); reset->hide();
        options=new QToolButton(controls); options->setText("⋯"); options->setToolTip("Preview options");
        options->setAccessibleName("Preview options");
        remove=new QToolButton(controls); remove->setText("×"); remove->setToolTip("Remove this preview");
        for (auto* button:{fit,reset,options,remove}) { button->setFixedSize(button==reset?50:32,28); button->setFocusPolicy(Qt::StrongFocus); buttons->addWidget(button); }
        selectBox=new PreviewCheckBox(controls); selectBox->setFixedSize(28,28);
        selectBox->setObjectName("selectPreview"); selectBox->setAccessibleName("Select preview: "+title);
        selectBox->setToolTip("Select for Close selected · Ctrl+click to toggle · Shift+click for a range");
        buttons->addWidget(selectBox);
        footer=new LoadingLabel(this); footer->setTextFormat(Qt::PlainText); footer->setAttribute(Qt::WA_NativeWindow,native);
        footer->setTextInteractionFlags(Qt::TextSelectableByMouse);
        footer->setStyleSheet("color:#e8edf3;background:#20262e;border:1px solid #414b57;border-radius:3px;padding:2px 5px;");
        triangles=new QLabel(this); triangles->setTextFormat(Qt::PlainText); triangles->setAttribute(Qt::WA_NativeWindow,native);
        triangles->setStyleSheet(footer->styleSheet()); triangles->setAccessibleName("Triangle count"); triangles->hide();
        if (playbackKind(kind)) {
            playback=new VideoControls(this,kind=="audio");
            if (kind=="audio") playback->setStyleSheet(style);
        }
        if (kind=="model") { animation=new AnimationControls(this); animation->hide(); }
        idle.setSingleShot(true); idle.setInterval(700);
        connect(&idle,&QTimer::timeout,this,[this] {
            if (!videoReady || !isVisible() || QGuiApplication::mouseButtons()!=Qt::NoButton ||
                QApplication::activePopupWidget() || QApplication::activeModalWidget()) return;
            auto* focused=QApplication::focusWidget();
            if (keyboardControls && focused && playback->isAncestorOf(focused)) return;
            for (auto* overlay:{header,controls,static_cast<QFrame*>(playback)}) {
                const auto pointer=overlay->mapFromGlobal(QCursor::pos());
                if (overlay->isVisible() && (overlay->mask().isEmpty()?overlay->rect().contains(pointer):overlay->mask().contains(pointer))) return;
            }
            conceal();
        });
        if (video) connect(qApp,&QApplication::focusChanged,this,[this](QWidget* before,QWidget* after) {
            if ((before && playback->isAncestorOf(before)) || (after && playback->isAncestorOf(after))) activity();
        });
    }
    ~PreviewCard() override { disconnect(qApp,nullptr,this,nullptr); }
    void setSurface(QWidget* widget,QWindow* native=nullptr) { surface=widget; nativeSurface=native; content->addWidget(widget); updateOverlays(); place(); }
    void activity(bool userInput=false) {
        if (!video || idleTransition || (awaitingFullscreenInput && !userInput)) return;
        if (userInput) awaitingFullscreenInput=false;
        idleHidden=false; unsetCursor();
        if (nativeSurface) nativeSurface->unsetCursor();
        updateOverlays();
        if (videoReady && isVisible()) idle.start();
        else idle.stop();
    }
    void inputActivity(QObject* object,QEvent* event) {
        if (!video) return;
        switch (event->type()) {
        case QEvent::MouseMove: {
            const auto position=static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
            if (position==pointerPosition) break;
            pointerPosition=position;
            keyboardControls=false; activity(true); break;
        }
        case QEvent::MouseButtonPress: case QEvent::Wheel:
            keyboardControls=false; activity(true); break;
        case QEvent::MouseButtonRelease: case QEvent::Enter:
            activity(); break;
        case QEvent::Leave: activity(); break;
        case QEvent::FocusIn: {
            const auto reason=static_cast<QFocusEvent*>(event)->reason();
            if (reason==Qt::TabFocusReason || reason==Qt::BacktabFocusReason || reason==Qt::ShortcutFocusReason) {
                keyboardControls=true;
                activity(true);
            } else activity();
            break;
        }
        case QEvent::KeyPress:
            if (auto* widget=qobject_cast<QWidget*>(object); widget && playback->isAncestorOf(widget)) keyboardControls=true;
            activity(true); break;
        default: break;
        }
    }
    void setPlayback(const QJsonObject& settings,const QJsonObject& metrics,std::shared_ptr<const preview::AudioWaveform> waveform={}) {
        playback->setWaveform(std::move(waveform));
        playback->setPlayback(settings,metrics);
        if (!video) return;
        const bool ready=metrics["engine"].toBool(), nextPaused=settings["paused"].toBool();
        const bool changed=ready!=videoReady || nextPaused!=paused;
        videoReady=ready; paused=nextPaused;
        if (changed) activity();
    }
    void setSelected(bool selected) {
        setAccessibleName((selected?"Active preview: ":"Preview: ")+fullTitle);
    }
    void setAnimation(const QJsonObject& settings,const QJsonObject& metrics) {
        if (!animation) return;
        animation->setPlayback(settings,metrics);
        const bool available=!metrics["animations"].toArray().isEmpty();
        if (animationAvailable!=available) { animationAvailable=available; updateOverlays(); place(); }
    }
    void setFullscreen(bool value) {
        setFrameShape(value?QFrame::NoFrame:QFrame::StyledPanel);
        if (playback) playback->setFullscreen(value);
        if (inFullscreen==value) return;
        inFullscreen=value;
        if (!video) return;
        awaitingFullscreenInput=value;
        if (value) { pointerPosition=QCursor::pos(); conceal(); }
        else activity();
    }
    void setStatus(const QString& text,const QString& tooltip) {
        static_cast<LoadingLabel*>(footer)->setLoading(text.startsWith("Loading") || text.startsWith("Building"));
        statusText=text; footer->setToolTip(tooltip); updateStatus();
    }
    void updateStatus() { updateOverlays(); place(); }
    void updateTriangles(const QJsonObject& config,const QJsonObject& metrics) {
        const auto kind=config["kind"].toString();
        const bool visible=(kind=="model" || kind=="material") && metrics["engine"].toBool() &&
            config["settings"].toObject()["triangles"].toBool(true);
        const auto count=metrics["triangles"];
        triangleText=count.isDouble()?QString("%1 tris").arg(compactTriangleCount(count.toInteger())):QString("Tris —");
        triangles->setToolTip(count.isDouble()?QString("%1 tris\nLoaded geometry · source triangles (including instances)").arg(QLocale().toString(count.toInteger())):
            "Triangle count is available for GLB, glTF, OBJ and material samples.");
        triangles->setVisible(visible); place();
    }
    QJsonObject overlays(QWidget* window) const {
        QJsonObject result;
        for (const auto& item: {std::pair{"name",static_cast<QWidget*>(header)}, {"controls",static_cast<QWidget*>(controls)},
                               {"selection",static_cast<QWidget*>(selectBox)},
                               {"status",static_cast<QWidget*>(footer)}, {"triangles",static_cast<QWidget*>(triangles)},
                               {"playback",static_cast<QWidget*>(playback)},{"animation",static_cast<QWidget*>(animation)}}) {
            if (!item.second) continue;
            const auto p=item.second->mapTo(window,QPoint());
            result[item.first]=QJsonObject{{"x",p.x()},{"y",p.y()},{"width",item.second->width()},{"height",item.second->height()},
                {"visible",item.second->isVisible()},{"loading",item.second->property("loading").toBool()}};
        }
        auto selection=result["selection"].toObject(); selection["checked"]=selectBox->isChecked(); result["selection"]=selection;
        return result;
    }
    void compose(QPainter& painter, QWidget* window) {
        for (auto* widget: {static_cast<QWidget*>(header),static_cast<QWidget*>(controls),static_cast<QWidget*>(footer),
                           static_cast<QWidget*>(triangles),static_cast<QWidget*>(playback),static_cast<QWidget*>(animation)})
            if (widget && widget->isVisible()) {
                const auto origin=widget->mapTo(window,QPoint());
                painter.save();
                if (!widget->mask().isEmpty()) painter.setClipRegion(widget->mask().translated(origin),Qt::IntersectClip);
                painter.drawPixmap(origin,widget->grab()); painter.restore();
            }
    }
    QToolButton *fit, *reset, *options, *remove;
    QCheckBox* selectBox;
    VideoControls* playback=nullptr;
    AnimationControls* animation=nullptr;
    bool model=false;
    QLabel* footer;
protected:
    bool event(QEvent* event) override {
        const bool result=QFrame::event(event);
        if (event->type()==QEvent::LayoutRequest && surface) place();
        return result;
    }
    void resizeEvent(QResizeEvent* event) override { QFrame::resizeEvent(event); place(); }
    void showEvent(QShowEvent* event) override {
        QFrame::showEvent(event); QTimer::singleShot(0,this,[this] { place(); activity(); });
    }
    void hideEvent(QHideEvent* event) override { idle.stop(); QFrame::hideEvent(event); }
private:
    void conceal() {
        idle.stop(); idleTransition=true; idleHidden=true;
        auto* focused=QApplication::focusWidget();
        if (focused && playback->isAncestorOf(focused) && surface) surface->setFocus(Qt::OtherFocusReason);
        updateOverlays(); setCursor(Qt::BlankCursor);
        if (nativeSurface) nativeSurface->setCursor(Qt::BlankCursor);
        idleTransition=false;
    }
    void updateOverlays() {
        header->setVisible(!idleHidden); controls->setVisible(!idleHidden);
        if (playback) playback->setVisible(!idleHidden);
        if (animation) animation->setVisible(animationAvailable);
        const bool steady=statusText.startsWith("Live");
        footer->setVisible(!idleHidden || !steady);
    }
    void place() {
        name->ensurePolished(); footer->ensurePolished(); triangles->ensurePolished(); controls->ensurePolished();
        const int inset=6;
        const bool dense=width()<300;
        fit->setVisible(!dense && !playback); remove->setVisible(!dense);
        reset->setVisible(model && !dense);
        const int headerHeight=34;
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
        if (playback) {
            playback->setCondensed(height()<145);
            const int barHeight=playback->sizeHint().height();
            playback->setGeometry(inset,std::max(inset,height()-inset-barHeight),badgeWidth,barHeight);
            playback->layout()->activate(); playback->raise();
        }
        if (animation && animationAvailable) {
            animation->setCondensed(height()<145);
            const int barHeight=animation->sizeHint().height();
            animation->setGeometry(inset,std::max(inset,height()-inset-barHeight),badgeWidth,barHeight);
            animation->layout()->activate(); animation->raise();
            triangles->move(inset,std::max(inset,animation->y()-28));
        }
    }
    QString fullTitle, statusText, triangleText;
    QFrame *header, *controls;
    QLabel *name, *triangles;
    QVBoxLayout* content;
    bool video=false, videoReady=false, paused=false, idleHidden=false, keyboardControls=false;
    bool idleTransition=false;
    bool inFullscreen=false, awaitingFullscreenInput=false;
    bool animationAvailable=false;
    QPoint pointerPosition=QCursor::pos();
    QTimer idle;
    QWindow* nativeSurface=nullptr;
    QWidget* surface=nullptr;
};
}

Service::Service(QString sock, QString state) : socketPath(std::move(sock)), statePath(std::move(state)) {
    cache.setDiagnosticsDirectory(statePath);
    projectPreloadDelay.setSingleShot(true); projectPreloadDelay.setInterval(250);
    connect(&projectPreloadDelay,&QTimer::timeout,this,[this] { preloadReturnTarget(true); });
    qApp->installEventFilter(this);
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
bool Service::eventFilter(QObject* watched, QEvent* event) {
    // Qt refreshes its separate tooltip palette on desktop theme changes.
    // Sync just before showing a tooltip, preserving a forced app appearance.
    if (event->type()==QEvent::ToolTip && theme!="auto") QToolTip::setPalette(QApplication::palette());
    return QObject::eventFilter(watched,event);
}
bool Service::start(QString& error) {
    QElapsedTimer clock; clock.start();
    thumbnailDirectory=preview::prepareThumbnailJobs(QFileInfo(socketPath).absolutePath());
    QFile state(statePath + "/previews.json");
    if (state.exists()) {
        if (!state.open(QIODevice::ReadOnly) || state.size() > 4*1024*1024) { error="Cannot read preview state"; return false; }
        QJsonParseError parse;
        auto doc=QJsonDocument::fromJson(state.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject() || doc.object()["version"].toInt() != 1 || !doc.object()["entries"].isArray()) {
            error="Invalid preview state; preserved at " + state.fileName(); return false;
        }
        if (doc.object().contains("theme")) {
            const QJsonValue saved=doc.object()["theme"];
            if (!saved.isString() || !Theme::valid(saved.toString())) { error="Invalid saved theme"; return false; }
            theme=saved.toString();
        }
        if (doc.object().contains("preferences") && !doc.object()["preferences"].isObject()) { error="Invalid saved preferences"; return false; }
        if (doc.object().contains("close_behavior")) {
            const QJsonValue saved=doc.object()["close_behavior"];
            if (!saved.isString() || (saved!="left" && saved!="last")) { error="Invalid saved close behavior"; return false; }
            closeBehavior=saved.toString();
        }
        preferences=doc.object()["preferences"].toObject();
        for (auto i=preferences.begin();i!=preferences.end();++i) {
            if (!QSet<QString>{"3d","image","video","audio"}.contains(i.key()) || !i.value().isObject()) { error="Invalid saved preference group"; return false; }
            QJsonObject config{{"path",statePath+"/preference-validation"},{"kind",i.key()=="3d"?"model":i.key()},{"settings",i.value()}};
            const auto invalid=validate(config,true);
            if (!invalid.isEmpty()) { error="Invalid saved preferences: "+invalid; return false; }
        }
        for (const auto& row : doc.object()["entries"].toArray()) {
            auto result=add(row.toObject(), true);
            if (!result["ok"].toBool()) { error="Invalid saved preview: " + result["error"].toString(); return false; }
        }
        layout=doc.object()["layout"].toString("single");
        if (layout != "single" && layout != "grid" && layout != "overview" && layout != "projects") { error="Invalid saved layout"; return false; }
        gridSize=doc.object()["grid_size"].toInt(2);
        if (gridSize<2 || gridSize>4 || (doc.object().contains("grid_size") && doc.object()["grid_size"].toDouble()!=gridSize)) { error="Invalid saved grid size"; return false; }
        const auto state=doc.object();
        if (state.contains("projects") && !state["projects"].isArray()) { error="Invalid saved projects"; return false; }
        if (state["projects"].toArray().size()>256) { error="Project limit reached (256)"; return false; }
        for (const auto& value:state["projects"].toArray()) {
            const auto project=value.toObject(); const auto id=project["id"].toString();
            static const QRegularExpression valid("^[A-Za-z0-9_-]{1,64}$");
            if (!valid.match(id).hasMatch() || projectOrder.contains(id) || !project["label"].isString() ||
                project["label"].toString().trimmed().isEmpty() || project["label"].toString().size()>256 || !project["current"].isString()) { error="Invalid saved project"; return false; }
            const auto current=project["current"].toString();
            if (!current.isEmpty() && (!entries.contains(current) || entries[current]->config.value("project")!=id)) { error="Invalid saved project current preview"; return false; }
            projects[id]=QJsonObject{{"label",project["label"]},{"current",current}}; projectOrder.append(id);
        }
        // Legacy raw clients may have project membership without a registry.
        for (const auto& id:order) {
            const auto project=entries[id]->config.value("project").toString();
            if (!project.isEmpty() && !projects.contains(project)) {
                if (projects.size()>=256) { error="Project limit reached (256)"; return false; }
                projects[project]=QJsonObject{{"label",project},{"current",id}}; projectOrder.append(project);
            }
        }
        if ((state.contains("group_projects") && !state["group_projects"].isBool()) ||
            (state.contains("project_filter") && !state["project_filter"].isString())) { error="Invalid saved project filters"; return false; }
        groupByProject=state["group_projects"].toBool(true); projectFilterId=state["project_filter"].toString();
        if (!projectFilterId.isEmpty() && projectFilterId!="@ungrouped" && !projects.contains(projectFilterId)) { error="Unknown saved project filter"; return false; }
        projectGridSize=state["project_grid_size"].toInt(0);
        if ((state.contains("project_grid_size") && (!state["project_grid_size"].isDouble() || state["project_grid_size"].toDouble()!=projectGridSize)) ||
            (projectGridSize!=0 && (projectGridSize<2 || projectGridSize>4))) { error="Invalid saved project grid size"; return false; }
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
    if (entries.contains(selected) && entries[selected]->config.value("inspection").toObject()["enabled"].toBool()) inspectionId=selected;
    runtimeTiming["restore_ms"]=clock.nsecsElapsed()/1e6;
    Theme::apply(theme);
    QLocalServer::removeServer(socketPath);
    if (!server.listen(socketPath)) { error=server.errorString(); return false; }
    runtimeTiming["service_start_ms"]=clock.nsecsElapsed()/1e6;
    return true;
}
void Service::save() {
    QJsonArray rows;
    for (const auto& id : order) rows.append(entries[id]->config);
    QJsonArray projectState;
    for (const auto& id:projectOrder) projectState.append(QJsonObject{{"id",id},{"label",projects.value(id).toObject()["label"]},{"current",projectCurrent(id)}});
    QSaveFile file(statePath + "/previews.json");
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error(file.errorString().toStdString());
    file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    auto data=QJsonDocument(QJsonObject{{"version",1},{"entries",rows},{"selected",selected},{"layout",layout},{"grid_size",gridSize},{"preferences",preferences},
        {"comparison",QJsonArray::fromStringList(comparison)},{"camera_sync",cameraSync},{"theme",theme},{"close_behavior",closeBehavior},
        {"projects",projectState},{"project_grid_size",projectGridSize},{"group_projects",groupByProject},{"project_filter",projectFilterId}}).toJson();
    if (file.write(data) != data.size() || !file.commit()) throw std::runtime_error("Cannot save preview registry");
    // Keep the in-memory fallback identical to the committed one. A later
    // no-current reconnect of a removed ID must not resurrect an old pointer.
    for (const auto& value:projectState) {
        const auto project=value.toObject(); const auto id=project["id"].toString();
        auto metadata=projects.value(id).toObject(); metadata["current"]=project["current"]; projects[id]=metadata;
    }
}
QString Service::validate(QJsonObject& c, bool restore) {
    if (QJsonDocument(c).toJson(QJsonDocument::Compact).size()>8192) return "Preview configuration exceeds 8 KiB";
    if (c.contains("codex")) {
        const auto error=validateCodexOrigin(c["codex"]); if (!error.isEmpty()) return error;
    }
    if (c.contains("project") && (!c.value("project").isString() ||
        (!c.value("project").toString().isEmpty() && !QRegularExpression("^[A-Za-z0-9_-]{1,64}$").match(c.value("project").toString()).hasMatch())))
        return "Project ID must use 1..64 letters, digits, underscores or hyphens";
    QString path=c["path"].toString();
    if (path.isEmpty() || !QFileInfo(path).isAbsolute()) return "An absolute output file path is required";
    if (!restore && QFileInfo(path).isDir()) return "Output must be a file";
    c["path"]=absolute(path);
    QString kind=c["kind"].toString("auto");
    if (kind == "auto") kind=path.endsWith(".ptex",Qt::CaseInsensitive) || path.endsWith(".materialgraph.json",Qt::CaseInsensitive)?"graph":audioPath(path)?"audio":videoPath(path)?"video":imagePath(path)?"image":"model";
    if (kind != "image" && kind != "model" && kind != "material" && kind != "video" && kind != "audio" && kind != "graph") return "Kind must be image, model, material, video, audio or graph";
    if (!restore && !playbackKind(kind) && QFileInfo(path).size() > 256*1024*1024) return "Preview file exceeds 256 MiB";
    c["kind"]=kind;
    if (c.contains("inspection")) {
        if (kind!="model" || !c["inspection"].isObject()) return "inspection requires a model configuration";
        const auto options=c["inspection"].toObject();
        for (auto it=options.begin();it!=options.end();++it) {
            if (it.key()=="enabled") { if (!it.value().isBool()) return "inspection.enabled must be boolean"; }
            else if (it.key()=="review") {
                if (!it.value().isString() || !QFileInfo(it.value().toString()).isAbsolute() || it.value().toString().toUtf8().size()>4096 || it.value()==c["path"])
                    return "inspection.review requires a separate absolute file path";
            } else if (it.key()=="asset_id" || it.key()=="variant") {
                if (!it.value().isString() || it.value().toString().isEmpty() || it.value().toString().toUtf8().size()>(it.key()=="variant"?128:256)) return "Invalid inspection identity";
            } else return "Unknown inspection configuration field: "+it.key();
        }
        const auto suffix=QFileInfo(path).suffix().toLower();
        if (options["enabled"].toBool() && suffix!="glb" && suffix!="gltf")
            return "Component inspection currently supports GLB and glTF models";
    }
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
    const auto review=c.value("inspection").toObject().value("review").toString();
    if (!review.isEmpty()) {
        const auto identity=preview::inspection::pathIdentity(review);
        if (identity==preview::inspection::pathIdentity(path)) return "Review must be separate from the model output";
        for (const auto& source:sources)
            if (identity==preview::inspection::pathIdentity(source.toString())) return "Review files must not be generator inputs";
    }
    const auto inspection=c.value("inspection").toObject();
    if (inspection["enabled"].toBool()) {
        for (const auto& key:{"review","asset_id","variant"})
            if (inspection[key].toString().isEmpty()) return "Enabled inspection requires review, asset_id and variant; use inspect open";
    }
    auto maps=c.value("maps").toObject();
    for (auto i=maps.begin();i!=maps.end();++i) {
        if ((i.key()!="normal" && i.key()!="orm") || !i.value().isString() || !QFileInfo(i.value().toString()).isAbsolute()) return "Maps support absolute normal and ORM paths";
    }
    if (c.contains("maps") && !c.value("maps").isObject()) return "maps must be an object";
    if (c.contains("settings") && !c.value("settings").isObject()) return "settings must be an object";
    auto settings=c.value("settings").toObject();
    const QSet<QString> known{"grid","axes","edges","orthographic","nearest","light","background","shape","roughness","metallic","materials","textures","texture_quality","ovoxel_mode","lock_horizon","up_axis","front_axis","lighting","paused","muted","loop","triangles","animation","animation_speed","animation_stepped","animation_fps","preview_model","preview_shape"};
    for (auto i=settings.begin();i!=settings.end();++i) {
        if (!known.contains(i.key())) return "Unknown setting: " + i.key();
        if(i.key()=="preview_model") {
            const auto model=i.value().toString();
            if(kind!="graph" || !i.value().isString() || model.size()>4096 || (!model.isEmpty() && (!QFileInfo(model).isAbsolute() || !model.endsWith(".glb",Qt::CaseInsensitive) || QFileInfo(model).isDir())))return "Graph preview model requires an absolute GLB file path or empty string";
        }
        else if(i.key()=="preview_shape") {
            if(kind!="graph" || !QSet<QString>{"sphere","cube","plane"}.contains(i.value().toString()))return "Graph preview shape must be sphere, cube or plane";
        }
        else if (i.key()=="animation") {
            const double index=i.value().toDouble(-1);
            if (kind!="model" || !i.value().isDouble() || !std::isfinite(index) || index<0 || index>255 || std::floor(index)!=index)
                return "Animation clip requires a model and integer index from 0 to 255";
        }
        else if (i.key()=="animation_fps") {
            const double fps=i.value().toDouble();
            if (kind!="model" || !i.value().isDouble() || !std::isfinite(fps) || fps<1 || fps>120 || std::floor(fps)!=fps)
                return "Animation frame rate requires a model and integer from 1 to 120";
        }
        else if (i.key()=="animation_stepped") {
            if (kind!="model" || !i.value().isBool()) return "Stepped animation requires a model and boolean";
        }
        else if (i.key()=="animation_speed") {
            const double speed=i.value().toDouble();
            if (kind!="model" || !i.value().isDouble() || !std::isfinite(speed) || speed<.1 || speed>4)
                return "Animation speed requires a model and a value from 0.1 to 4";
        }
        else if (i.key()=="background") { if (!QSet<QString>{"dark","light","checker"}.contains(i.value().toString())) return "Invalid background"; }
        else if (i.key()=="shape") { if (!QSet<QString>{"sphere","cube","plane"}.contains(i.value().toString())) return "Invalid material shape"; }
        else if (i.key()=="front_axis") {
            if (kind!="model" || !QSet<QString>{"+x","-x","+y","-y","+z","-z"}.contains(i.value().toString())) return "Front axis requires a model and signed x, y or z";
        }
        else if (i.key()=="up_axis") { if (!QSet<QString>{"y","z"}.contains(i.value().toString())) return "Up axis must be y or z"; }
        else if (i.key()=="lighting") { if (!QSet<QString>{"studio","lightkit"}.contains(i.value().toString())) return "Lighting must be studio or lightkit"; }
        else if (i.key()=="ovoxel_mode") {
            if(kind!="model" || !QSet<QString>{"surface","cells","voxels","vertices"}.contains(i.value().toString()))
                return "O-Voxel mode requires a model and surface, cells, voxels or vertices";
        }
        else if (i.key()=="texture_quality") {
            if((kind!="model" && kind!="material") || !QSet<QString>{"original","preview"}.contains(i.value().toString()))
                return "Texture quality requires a 3D preview and original or preview";
        }
        else if (i.key()=="light" || i.key()=="roughness" || i.key()=="metallic") {
            if (!i.value().isDouble() || i.value().toDouble()<0 || i.value().toDouble()>(i.key()=="light"?5.0:1.0)) return "Invalid numeric setting";
        } else if (!i.value().isBool()) return "Boolean setting required: " + i.key();
    }
    if (c.contains("timeout") && (!c.value("timeout").isDouble() || c.value("timeout").toDouble()<1 || c.value("timeout").toDouble()>3600)) return "Timeout must be 1–3600 seconds";
    const auto front=settings.value("front_axis").toString();
    if (!front.isEmpty() && front.back()==settings.value("up_axis").toString("y").back()) return "Front axis must be perpendicular to up axis";
    c["command"]=command.toArray(); c["maps"]=maps; c["settings"]=settings;
    c["timeout"]=c.value("timeout").toDouble(120);
    return {};
}
QJsonObject Service::add(QJsonObject config, bool restore, bool makeCurrent, const QString& projectLabel) {
    const auto previousProjects=projects; const auto previousProjectOrder=projectOrder;
    bool committed=restore;
    const auto projectRollback=qScopeGuard([&] { if (!committed) { projects=previousProjects; projectOrder=previousProjectOrder; } });
    const auto oldComparison=comparison;
    const QString error=validate(config,restore);
    if (!error.isEmpty()) return failure(error);
    const QString id=config["id"].toString();
    if (!entries.contains(id) && entries.size()>=256) return failure("Preview limit reached (256)");
    auto prior=entries.value(id);
    const bool existing=bool(prior);
    QJsonObject old=prior?prior->config:QJsonObject{};
    if (!restore && config.value("inspection").toObject()["enabled"].toBool() && inspectionId!=id)
        return failure("Use inspect open to enter inspection before reconnecting an enabled registration");
    if (!restore) {
        if (!config.contains("codex") && old.contains("codex")) config["codex"]=old["codex"];
        if (!config.contains("project") && old.contains("project")) config["project"]=old.value("project");
        auto settings=preferences.value(preferenceGroup(config["kind"].toString())).toObject();
        // Reconnecting the same asset keeps its inspection choices. Explicit
        // registration options still win over remembered preferences.
        if (existing && old["kind"]==config["kind"]) {
            if (!config.contains("camera") && old.contains("camera")) config["camera"]=old["camera"];
            if (!config.contains("image_view") && old.contains("image_view")) config["image_view"]=old["image_view"];
            if (!config.contains("inspection") && old["path"]==config["path"] && old.contains("inspection")) config["inspection"]=old["inspection"];
            const auto previous=old["settings"].toObject();
            for (auto i=previous.begin();i!=previous.end();++i) settings[i.key()]=i.value();
        }
        const auto explicitSettings=config["settings"].toObject();
        for (auto i=explicitSettings.begin();i!=explicitSettings.end();++i) settings[i.key()]=i.value();
        config["settings"]=settings;
        if (existing && preview::frameChanged(old["settings"].toObject(),settings)) config.remove("camera");
        const auto invalid=validate(config);
        if (!invalid.isEmpty()) return failure(invalid);
    }
    const auto project=config.value("project").toString();
    if (!restore && !project.isEmpty()) {
        if (!projects.contains(project) && projects.size()>=256) return failure("Project limit reached (256)");
        auto metadata=projects.value(project).toObject();
        if (!projects.contains(project)) { metadata["label"]=project; projectOrder.append(project); }
        if (!projectLabel.isEmpty()) metadata["label"]=projectLabel;
        if (makeCurrent || metadata["current"].toString().isEmpty()) metadata["current"]=id;
        projects[project]=metadata;
    }
    const bool projectChanged=projects!=previousProjects;
    auto unchanged=[&]() {
        if (projectChanged) save();
        committed=true;
        if (projectChanged) reconcile();
        return success({{"id",id}});
    };
    if (!restore && prior && config!=old) {
        auto oldContent=old, nextContent=config;
        for (const auto& key:{"codex","project"}) { oldContent.remove(key); nextContent.remove(key); }
        if (oldContent==nextContent) {
            prior->config=config;
            try { save(); } catch (...) { prior->config=old; throw; }
            committed=true; if (projectChanged || old.value("project")!=config.value("project")) reconcile();
            return success({{"id",id}});
        }
    }
    if (!restore && prior && config==old) {
        if (!prior->active) return unchanged();
        const bool building=prior->process && prior->process->state()!=QProcess::NotRunning;
        bool inputsCurrent=true;
        for (const auto& path:config["watch"].toArray())
            if (prior->stamps.value(path.toString())!=stamp(path.toString())) { inputsCurrent=false; break; }
        if (!prior->retries && inputsCurrent && (prior->status.startsWith("Live") || building) && currentPublication(*prior))
            return unchanged();
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
        committed=true;
        if (existing) deactivate(id);
        if (existing && preview::frameChanged(old["settings"].toObject(),config["settings"].toObject())) prior->inspectionView.remove("camera");
        if (old.value("inspection").toObject().value("review")!=config.value("inspection").toObject().value("review")) {
            prior->reviewDocument={}; prior->reviewDigest.clear(); prior->reviewStamp.clear();
            prior->reviewEstablished=false; prior->reviewRevision=0;
            prior->markerRevision.clear(); prior->markerDigest.clear();
        }
        if (inspectionId==id && !config.value("inspection").toObject()["enabled"].toBool()) inspectionId.clear();
        reconcile();
        if(existing && window)window->refreshOverview();
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
        if (const auto* playback=static_cast<PreviewCard*>(e.card)->playback) {
            const auto videoControls=playback->describe(window.get());
            for (auto i=videoControls.begin();i!=videoControls.end();++i) controls[i.key()]=i.value();
        }
        if (const auto* animation=static_cast<PreviewCard*>(e.card)->animation) {
            const auto fields=animation->describe(window.get());
            for (auto i=fields.begin();i!=fields.end();++i) controls[i.key()]=i.value();
        }
        result["controls"]=controls;
        result["status_visible"]=e.statusLabel->isVisible();
        result["overlays"]=static_cast<PreviewCard*>(e.card)->overlays(window.get());
    }
    return result;
}
QJsonObject Service::select(const QString& id,bool updateViews,bool single,bool makeCurrent) {
    const auto e=entries.value(id);
    if(!e) return failure("Unknown preview: "+id);
    const auto project=e->config.value("project").toString();
    if (makeCurrent && (project.isEmpty() || !projects.contains(project))) return failure("Current preview must belong to a project");
    const auto oldProject=projects.value(project);
    const bool promote=makeCurrent && oldProject.toObject()["current"]!=id;
    if (!promote && selected==id && (!single || (layout=="single" && comparison.isEmpty() && inspectionId.isEmpty()))) return success();
    if (window && !window->commitInspectionControls()) return failure("Could not apply the pending cut adjustment");
    const auto oldInspectionId=inspectionId;
    const auto inspected=entries.value(oldInspectionId);
    const auto inspectedConfig=inspected?inspected->config:QJsonObject{};
    QJsonObject departingView;
    if (inspected) {
        if (inspected->active && inspected->view) {
            QString error; const auto state=inspected->view->inspect({{"action","state"}},error);
            if (error.isEmpty()) {
                for (const auto& key:{"cuts","hidden","selected","selection","revision","dependency_revision","orthographic"})
                    if (state.contains(key)) departingView[key]=state[key];
                const auto camera=inspected->view->cameraState();
                if (!camera.isEmpty()) { departingView["camera"]=camera; inspected->config["camera"]=camera; }
            }
        }
        auto options=inspected->config.value("inspection").toObject(); options["enabled"]=false;
        inspected->config["inspection"]=options;
    }
    inspectionId.clear();
    const auto previous=selected; selected=id;
    const auto previousLayout=layout;
    if (single) layout="single";
    const auto oldComparison=comparison;
    if (single || (!comparison.isEmpty() && !comparison.contains(id))) comparison.clear();
    if (promote) { auto metadata=oldProject.toObject(); metadata["current"]=id; projects[project]=metadata; }
    try { save(); } catch (...) {
        selected=previous; layout=previousLayout; comparison=oldComparison; inspectionId=oldInspectionId;
        if (promote) projects[project]=oldProject;
        if (inspected) inspected->config=inspectedConfig;
        throw;
    }
    if (inspected && !departingView.isEmpty()) inspected->inspectionView=departingView;
    if (previous!=id && !previous.isEmpty()) {
        selectionHistory.removeAll(previous);
        selectionHistory.append(previous);
        selectionHistory.removeAll(id);
    }
    // A visible card only needs its selection state updated.
    // Retain its existing layout and native input focus.
    if(updateViews) {
        if (!promote && oldInspectionId.isEmpty() && previousLayout==layout && e->active && comparison==oldComparison && window) {
            window->navigation(); preloadReturnTarget(); watchers();
        }
        else reconcile();
    }
    return success();
}
QJsonObject Service::request(const QJsonObject& r) {
    const auto rememberCrashContext = [this,&r] {
        QJsonArray previews;
        for (const auto& key:order) {
            const auto& entry=*entries[key];
            if (!entry.active && key!=selected && !comparison.contains(key)) continue;
            previews.append(QJsonObject{{"id",key},{"path",entry.config["path"]},{"kind",entry.config["kind"]},
                {"settings",entry.config["settings"]},{"status",entry.status},{"active",entry.active}});
        }
        QJsonObject state{{"selected",selected},{"comparison",QJsonArray::fromStringList(comparison)},{"layout",layout},{"previews",previews}};
        const auto method=r["method"].toString();
        if (method!="ping" && method!="list") state["last_request"]=QJsonObject{{"method",method},{"id",r["id"]}};
        preview::CrashLog::context(state);
    };
    rememberCrashContext();
    const auto crashContextGuard=qScopeGuard(rememberCrashContext);
    try {
        const QString method=r["method"].toString(), id=r["id"].toString();
        if (r.contains("codex")) {
            const auto error=validateCodexOrigin(r["codex"]); if (!error.isEmpty()) return failure(error);
        }
        if (method=="ping") return success({{"pid",qint64(QCoreApplication::applicationPid())},{"protocol",1},{"platform",QGuiApplication::platformName()}});
        if (method=="list") {
            QJsonArray rows; int active=0;
            for (const auto& key:order) { rows.append(describe(*entries[key])); active+=entries[key]->active; }
            return success({{"entries",rows},{"selected",selected},{"layout",layout},{"active",active},{"grid_size",gridSize},
                {"presentation",window?window->presentation():QJsonObject{}},{"performance",runtimeTiming},{"cache",cache.metrics()},
                {"projects",projectRows()},{"project_grid_size",projectGridSize},{"group_projects",groupByProject},{"project_filter",projectFilterId},
                {"window_visible",bool(window && window->isVisible())},{"watched_files",watcher.files().size()},
                {"watched_directories",watcher.directories().size()},{"pid",qint64(QCoreApplication::applicationPid())},
                 {"platform",QGuiApplication::platformName()},{"options_open",bool(QApplication::activePopupWidget())},{"preferences",preferences},
                 {"theme",theme},{"effective_theme",Theme::effective()},{"close_behavior",closeBehavior},
                 {"close_target",closeTarget(selected)},{"preload",preloader.metrics()},
                 {"comparison",QJsonArray::fromStringList(comparison)},{"camera_sync",cameraSync},{"closed_count",closedEntries.size()}});
        }
        if (method=="cache") {
            const auto action=r["action"].toString("stats");
            if (action!="stats" && action!="clear" && action!="events") return failure("Cache action must be stats, events or clear");
            if (action=="events") {
                const int limit=r["limit"].toInt(64); const double after=r["after"].toDouble(0);
                if ((r.contains("limit") && (!r["limit"].isDouble() || r["limit"].toDouble()!=limit)) || limit<1 || limit>128 ||
                    (r.contains("after") && !r["after"].isDouble()) || !std::isfinite(after) || after<0 || after>9007199254740991.0 || std::floor(after)!=after)
                    return failure("Cache events require limit 1..128 and a nonnegative integer after sequence");
                auto result=cache.events(limit,quint64(after)); result["cache"]=cache.metrics(); return success(result);
            }
            if (action=="clear") { projectPreloadDelay.stop(); preloader.target(); cache.clear(true); watchers(); if (window) window->refreshOverview(true); }
            return success({{"cache",cache.metrics()}});
        }
        if (method=="close_behavior") {
            if (r.contains("mode")) {
                const auto mode=r["mode"];
                if (!mode.isString() || (mode!="left" && mode!="last")) return failure("Close behavior must be left or last");
                const auto previous=closeBehavior; closeBehavior=mode.toString();
                try { save(); } catch (...) { closeBehavior=previous; throw; }
                preloadReturnTarget(); watchers();
            }
            return success({{"close_behavior",closeBehavior},{"close_target",closeTarget(selected)}});
        }
        if (method=="theme") {
            if (r.contains("mode")) {
                if (!r["mode"].isString() || !Theme::valid(r["mode"].toString())) return failure("Theme must be auto, dark or light");
                const auto previous=theme;
                theme=r["mode"].toString();
                try { save(); } catch (...) { theme=previous; throw; }
                Theme::apply(theme);
            }
            return success({{"theme",theme},{"effective_theme",Theme::effective()}});
        }
        if (method=="defaults") {
            const auto group=r["group"].toString();
            if (!QSet<QString>{"image","video","audio","3d"}.contains(group)) return failure("Invalid defaults group");
            if (!r["settings"].isObject()) return failure("settings must be an object");
            auto remembered=preferences.value(group).toObject();
            const auto patch=r["settings"].toObject();
            if (patch.contains("front_axis")) return failure("Front axis is a per-model source setting");
            if (patch.contains("ovoxel_mode")) return failure("O-Voxel view is a per-model setting");
            if (patch.contains("animation") || patch.contains("animation_speed") || patch.contains("animation_stepped") || patch.contains("animation_fps")) return failure("Animation transport choices are per-model settings");
            for (auto i=patch.begin();i!=patch.end();++i) remembered[i.key()]=i.value();
            QJsonObject config{{"path",statePath+"/preference-validation"},{"kind",group=="3d"?"model":group},{"settings",remembered}};
            const auto error=validate(config); if (!error.isEmpty()) return failure(error);
            const auto old=preferences; preferences[group]=remembered;
            try { save(); } catch (...) { preferences=old; throw; }
            return success();
        }
        if (method=="project") {
            if (r["action"].toString()=="list") return success({{"projects",projectRows()}});
            if (!QRegularExpression("^[A-Za-z0-9_-]{1,64}$").match(id).hasMatch()) return failure("Invalid project ID");
            if (r.contains("label") && (!r["label"].isString() || r["label"].toString().trimmed().isEmpty() || r["label"].toString().size()>256)) return failure("Project label must contain 1..256 characters");
            if (r.contains("current") && (!r["current"].isString() || !entries.contains(r["current"].toString()) || entries[r["current"].toString()]->config.value("project")!=id)) return failure("Current preview must belong to the project");
            const auto action=r["action"].toString("upsert");
            if (action!="upsert" && action!="remove") return failure("Project action must be upsert, remove or list");
            if (action=="remove") {
                if (!projects.contains(id)) return failure("Unknown project");
                for (const auto& entry:entries) if (entry->config.value("project")==id) return failure("Remove a project's previews before removing the project");
            } else if (!projects.contains(id) && projects.size()>=256) return failure("Project limit reached (256)");
            const auto oldProjects=projects; const auto oldOrder=projectOrder; const auto oldFilter=projectFilterId;
            if (action=="remove") { projects.remove(id); projectOrder.removeAll(id); if (projectFilterId==id) projectFilterId.clear(); }
            else {
                auto metadata=projects.value(id).toObject();
                if (!projects.contains(id)) { metadata={{"label",id},{"current",""}}; projectOrder.append(id); }
                for (const auto& key:{"label","current"}) if (r.contains(key)) metadata[key]=r[key];
                projects[id]=metadata;
            }
            try { save(); } catch (...) { projects=oldProjects; projectOrder=oldOrder; projectFilterId=oldFilter; throw; }
            reconcile(); return success({{"id",id},{"projects",projectRows()}});
        }
        if (method=="add") {
            if (r.contains("make_current") && !r["make_current"].isBool()) return failure("make_current must be boolean");
            if (r.contains("project_label") && (!r["project_label"].isString() || r["project_label"].toString().trimmed().isEmpty() || r["project_label"].toString().size()>256)) return failure("Invalid project label");
            return add(r["entry"].toObject(),false,r["make_current"].toBool(true),r["project_label"].toString());
        }
        if (method=="inspect") return inspectionRequest(r);
        if (method=="annotations") return annotationRequest(r);
        if (method=="reopen") {
            if (!inspectionId.isEmpty()) return failure("Return to Preview before reopening another asset");
            if (closedEntries.isEmpty()) return failure("No closed previews to reopen");
            if (entries.size()>=256) return failure("Preview limit reached (256)");
            auto closed=closedEntries.last(); auto config=closed.config;
            const auto invalid=validate(config,true); if (!invalid.isEmpty()) return failure(invalid);
            const auto reopenId=config["id"].toString();
            if (entries.contains(reopenId)) { closedEntries.removeLast(); return request(r); }
            const auto oldOrder=order; const auto oldSelected=selected; const auto oldComparison=comparison;
            const auto oldProjects=projects; const auto oldProjectOrder=projectOrder;
            const auto project=config.value("project").toString();
            if (!project.isEmpty() && !projects.contains(project)) {
                if (projects.size()>=256) return failure("Project limit reached (256)");
                projects[project]=QJsonObject{{"label",project},{"current",reopenId}}; projectOrder.append(project);
            }
            auto entry=std::make_shared<Entry>(); entry->config=config; entries[reopenId]=entry;
            order.insert(std::clamp(closed.index,0,int(order.size())),reopenId); selected=reopenId; comparison.clear();
            try { save(); } catch (...) { projects=oldProjects; projectOrder=oldProjectOrder; entries.remove(reopenId); order=oldOrder; selected=oldSelected; comparison=oldComparison; throw; }
            if (config.value("inspection").toObject()["enabled"].toBool()) inspectionId=reopenId;
            closedEntries.removeLast(); selectionHistory.removeAll(reopenId);
            if (!oldSelected.isEmpty()) { selectionHistory.removeAll(oldSelected); selectionHistory.append(oldSelected); }
            reconcile(); return success({{"id",reopenId}});
        }
        if (method=="compare") {
            if (!inspectionId.isEmpty()) return failure("Return to Preview before starting a comparison");
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
            inspectionId.clear();
            // Persist first so failed saves leave running previews intact.
            for (const auto& id:previousOrder) deactivate(id);
            entries.clear(); cache.clear(false,"close_all"); reconcile(); return success();
        }
        if (method=="remove_many") {
            if (!r["ids"].isArray() || r["ids"].toArray().isEmpty() || r["ids"].toArray().size()>256)
                return failure("Close selected requires 1 to 256 preview IDs");
            QSet<QString> ids;
            for (const auto& value:r["ids"].toArray()) {
                if (!value.isString() || !entries.contains(value.toString())) return failure("Unknown preview in close selection");
                ids.insert(value.toString());
            }
            const auto previousOrder=order, previousComparison=comparison;
            const auto previousSelected=selected;
            for (const auto& id:ids) order.removeAll(id);
            if (ids.contains(selected)) {
                selected.clear();
                if (closeBehavior=="last") for (auto it=selectionHistory.crbegin();it!=selectionHistory.crend();++it)
                    if (order.contains(*it)) { selected=*it; break; }
                const int index=previousOrder.indexOf(previousSelected);
                for (int i=index-1;selected.isEmpty() && i>=0;--i)
                    if (order.contains(previousOrder[i])) selected=previousOrder[i];
                if (selected.isEmpty()) selected=order.value(0);
            }
            if (std::any_of(comparison.cbegin(),comparison.cend(),[&ids](const QString& id) { return ids.contains(id); })) comparison.clear();
            try { save(); } catch (...) { order=previousOrder; selected=previousSelected; comparison=previousComparison; throw; }
            // Remember positions in removal order so reverse-order reopening
            // restores the original ordering, including the active entry.
            QStringList closing;
            for (const auto& id:previousOrder) if (ids.contains(id) && id!=previousSelected) closing.append(id);
            if (ids.contains(previousSelected)) closing.append(previousSelected);
            auto remaining=previousOrder;
            for (const auto& id:closing) {
                closedEntries.append({entries[id]->config,int(remaining.indexOf(id))}); remaining.removeAll(id);
                selectionHistory.removeAll(id);
            }
            while (closedEntries.size()>32) closedEntries.removeFirst();
            selectionHistory.removeAll(selected);
            for (const auto& id:closing) { deactivate(id); entries.remove(id); }
            if (ids.contains(inspectionId)) inspectionId.clear();
            if (order.isEmpty()) cache.clear(false,"close_all");
            reconcile(); return success();
        }
        if (method=="open") {
            if (!r["paths"].isArray() || r["paths"].toArray().size()>256)
                return failure("Open requires an array of at most 256 file paths");
            QStringList paths;
            QMap<QString,QJsonObject> candidates;
            QMap<QString,QString> byPath;
            QSet<QString> allocated;
            for(const auto& id:order) {
                const auto path=entries[id]->config["path"].toString();
                if(!byPath.contains(path)) byPath.insert(path,id);
                allocated.insert(id);
            }
            // Validate the whole batch before registering anything. Reopening
            // an existing path must preserve its generator, settings and camera.
            for (const auto& value:r["paths"].toArray()) {
                if (!value.isString() || value.toString().isEmpty() || !QFileInfo(value.toString()).isAbsolute())
                    return failure("Open requires absolute file paths");
                const auto path=absolute(value.toString());
                if (QFileInfo(path).isDir()) return failure("Open requires files, not directories");
                paths.append(path);
                if (!byPath.contains(path)) {
                    QJsonObject config{{"path",path}};
                    if (r.contains("codex")) config["codex"]=r["codex"];
                    auto error=validate(config);
                    if (!error.isEmpty()) return failure(error);
                    config["settings"]=preferences.value(preferenceGroup(config["kind"].toString())).toObject();
                    error=validate(config);
                    if (!error.isEmpty()) return failure(error);
                    while(allocated.contains(config["id"].toString()))
                        config["id"]=QUuid::createUuid().toString(QUuid::Id128).left(12);
                    const auto id=config["id"].toString();
                    allocated.insert(id); byPath.insert(path,id);
                    candidates.insert(path,config);
                }
            }
            if (entries.size()+candidates.size()>256) return failure("Preview limit reached (256)");
            QJsonArray ids;
            QStringList added;
            const auto previousOrder=order;
            for (const auto& path:paths) {
                const auto id=byPath[path];
                if (!entries.contains(id)) {
                    auto entry=std::make_shared<Entry>(); entry->config=candidates[path];
                    entries.insert(id,entry); order.append(id); added.append(id);
                }
                ids.append(id);
            }
            auto rollback=[&] { for(const auto& id:added) entries.remove(id); order=previousOrder; };
            const bool changedSelection=!ids.isEmpty() && selected!=ids.last().toString();
            try {
                if (changedSelection) {
                    // Selection publishes the complete staged registry once,
                    // before it changes any live views or generator ownership.
                    const auto result=select(ids.last().toString(),false);
                    if (!result["ok"].toBool()) { rollback(); return result; }
                } else if(!added.isEmpty()) save();
            } catch(...) { rollback(); throw; }
            if (r.contains("codex")) codexOrigin=r["codex"].toObject();
            if(window && window->isVisible()) {
                window->present();
                if(!added.isEmpty() || changedSelection) reconcile();
            } else {
                const auto result=request({{"method","show"}});
                if (!result["ok"].toBool()) return result;
            }
            return success({{"ids",ids}});
        }
        if (method=="codex") {
            if (!window || !window->isVisible()) return failure("Open Preview before using its Codex sidebar");
            const auto action=r["action"].toString();
            if (action=="open") window->openCodex();
            else if (action=="collapse") window->collapseCodex();
            else return failure("Codex action must be open or collapse");
            return success();
        }
        if (method=="show") {
            if (r.contains("codex")) codexOrigin=r["codex"].toObject();
            if (!window) window=std::make_unique<PreviewWindow>(this);
            window->present(); reconcile(); return success();
        }
        if (method=="hide") {
            if (window && window->isVisible() && !window->isMinimized())
                for (const auto& entry:entries) if (entry->active) rememberThumbnail(*entry);
            if (window) window->hide();
            reconcile(); return success();
        }
        if (method=="quit") { QTimer::singleShot(0,qApp,&QCoreApplication::quit); return success(); }
        if (method=="layout") {
            if (r.contains("layout") && !r["layout"].isString()) return failure("layout must be a string");
            QString next=r["layout"].toString(layout);
            if (next!="single" && next!="grid" && next!="overview" && next!="projects") return failure("Layout must be single, grid, overview or projects");
            const int size=r["grid_size"].toInt(gridSize);
            if (r.contains("grid_size") && (!r["grid_size"].isDouble() || r["grid_size"].toDouble()!=size || size<2 || size>4)) return failure("Grid size must be 2, 3 or 4");
            if (r.contains("group_projects") && !r["group_projects"].isBool()) return failure("group_projects must be boolean");
            if (r.contains("project_filter") && (!r["project_filter"].isString() || (!r["project_filter"].toString().isEmpty() && r["project_filter"]!="@ungrouped" && !projects.contains(r["project_filter"].toString())))) return failure("Unknown project filter");
            const int projectSize=r["project_grid_size"].toInt(projectGridSize);
            if (r.contains("project_grid_size") && (!r["project_grid_size"].isDouble() || r["project_grid_size"].toDouble()!=projectSize || (projectSize!=0 && (projectSize<2 || projectSize>4)))) return failure("Project grid size must be 0 (auto), 2, 3 or 4");
            const auto oldProjectSize=projectGridSize; const auto oldFilter=projectFilterId; const auto oldGrouping=groupByProject;
            const auto previous=layout; const auto previousSize=gridSize;
            const auto oldComparison=comparison;
            const auto oldInspectionId=inspectionId;
            const auto inspected=entries.value(inspectionId);
            const auto inspectedConfig=inspected?inspected->config:QJsonObject{};
            if ((next=="overview" || next=="projects") && inspected) {
                if (window && !window->commitInspectionControls()) return failure("Could not apply the pending cut adjustment");
                auto options=inspected->config.value("inspection").toObject(); options["enabled"]=false;
                inspected->config["inspection"]=options; inspectionId.clear();
            }
            layout=next; gridSize=size; projectGridSize=projectSize;
            groupByProject=r["group_projects"].toBool(groupByProject); projectFilterId=r["project_filter"].toString(projectFilterId);
            if (r.contains("layout") || r.contains("grid_size")) comparison.clear();
            try { save(); } catch (...) {
                projectGridSize=oldProjectSize; projectFilterId=oldFilter; groupByProject=oldGrouping;
                layout=previous; gridSize=previousSize; comparison=oldComparison; inspectionId=oldInspectionId;
                if (inspected) inspected->config=inspectedConfig;
                throw;
            }
            reconcile(); return success();
        }
        if (method=="capture") {
            const QString path=r["path"].toString();
            if (!window || !window->isVisible()) return failure("Window must be open to capture");
            if (!QFileInfo(path).isAbsolute() || QFileInfo(path).exists()) return failure("Capture requires a new absolute file path");
            QImage capture=window->captureWindow();
            if (!capture.save(path,"PNG")) return failure("Cannot save capture");
            return success();
        }
        if (!entries.contains(id)) return failure("Unknown preview: " + id);
        auto e=entries[id];
        if (method=="remove") {
            const auto previousOrder=order; const auto previousSelected=selected;
            const auto oldComparison=comparison;
            const int index=order.indexOf(id);
            const auto destination=selected==id?closeTarget(id):selected;
            entries.remove(id); order.removeAll(id);
            if (comparison.contains(id)) comparison.clear();
            selected=destination;
            try { save(); } catch (...) {
                entries[id]=e; order=previousOrder; selected=previousSelected; comparison=oldComparison; throw;
            }
            closedEntries.append({e->config,index}); if (closedEntries.size()>32) closedEntries.removeFirst();
            selectionHistory.removeAll(id);
            selectionHistory.removeAll(selected);
            // Keep live views intact until the removal has been persisted.
            entries[id]=e; deactivate(id); entries.remove(id);
            if (inspectionId==id) inspectionId.clear();
            reconcile(); return success();
        }
        if (method=="select") {
            if (r.contains("make_current") && !r["make_current"].isBool()) return failure("make_current must be boolean");
            return select(id,true,false,r["make_current"].toBool());
        }
        if (method=="zoom") return select(id,true,true);
        if (method=="reload") {
            if (e->active) {
                if (e->view) e->view->reuseCachedPayload=false;
                if (!e->config["command"].toArray().isEmpty()) { e->forceGeneratorReload=true; build(id); }
                else reload(id);
                if (e->view) e->view->reuseCachedPayload=true;
            }
            return success();
        }
        if (method=="camera_view") {
            const auto view=r["view"].toString();
            if (!e->view || (e->config["kind"]!="model" && e->config["kind"]!="material")) return failure("Camera views require a visible 3D preview");
            if (!QSet<QString>{"front","back","left","right","top","bottom","isometric"}.contains(view)) return failure("Invalid camera view");
            e->view->cameraView(view); return success();
        }
        if (method=="seek") {
            if ((!playbackKind(e->config["kind"].toString()) && e->config["kind"]!="model") || !e->view)
                return failure("Seek requires visible audio, video or an animated model");
            if (!r["seconds"].isDouble() || !std::isfinite(r["seconds"].toDouble()) || r["seconds"].toDouble()<0)
                return failure("Seek requires finite nonnegative seconds");
            QString error; return e->view->seek(r["seconds"].toDouble(),error)?success():failure(error);
        }
        if(method=="animation_step") {
            const auto direction=r["direction"].toDouble();
            if(e->config["kind"]!="model" || !e->view || !e->view->metrics()["seekable"].toBool()) return failure("Frame stepping requires a visible animated model");
            if(!r["direction"].isDouble() || (direction!=-1 && direction!=1)) return failure("Frame step direction must be -1 or 1");
            const auto paused=request({{"method","settings"},{"id",id},{"settings",QJsonObject{{"paused",true}}}});
            if(!paused["ok"].toBool())return paused;
            QString error;
            return e->view->stepAnimation(int(direction),e->config["settings"].toObject()["animation_fps"].toInt(12),error)?success():failure(error);
        }
        if (method=="settings") {
            auto config=e->config, settings=config["settings"].toObject();
            const auto patch=r["settings"].toObject();
            if (!r["settings"].isObject()) return failure("settings must be an object");
            if (patch.contains("animation") && e->view) {
                const auto clips=e->view->metrics()["animations"].toArray();
                if (!clips.isEmpty() && patch["animation"].toInt(-1)>=clips.size()) return failure("Animation clip is not available in this model");
            }
            if (inspectionId==id && (patch.contains("animation") || patch.contains("animation_speed") || patch.contains("animation_stepped") || patch.contains("animation_fps") || patch.contains("paused") || patch.contains("loop")))
                return failure("Return to Preview to control animations; component inspection is static");
            for (auto i=patch.begin();i!=patch.end();++i) settings[i.key()]=i.value();
            config["settings"]=settings; QString error=validate(config);
            if (!error.isEmpty()) return failure(error);
            const auto old=e->config;
            if (preview::frameChanged(old["settings"].toObject(),settings)) config.remove("camera");
            e->config=config;
            const auto previousPreferences=preferences;
            const auto group=preferenceGroup(config["kind"].toString());
            auto remembered=preferences.value(group).toObject();
            for (auto i=patch.begin();i!=patch.end();++i) {
                const bool animationSetting=config["kind"]=="model" &&
                    (i.key()=="animation" || i.key()=="animation_speed" || i.key()=="animation_stepped" || i.key()=="animation_fps" || i.key()=="paused" || i.key()=="loop");
                if (i.key()!="front_axis" && i.key()!="ovoxel_mode" && !animationSetting) remembered[i.key()]=i.value();
            }
            preferences[group]=remembered;
            try { save(); } catch (...) { e->config=old; preferences=previousPreferences; throw; }
            if (preview::frameChanged(old["settings"].toObject(),settings)) e->inspectionView.remove("camera");
            if (e->view) e->view->settings(config);
            if (e->card) status(*e,e->status);
            if (window) { window->refreshOverview(); watchers(); }
            return success();
        }
        return failure("Unknown method");
    } catch (const std::exception& e) { return failure(QString::fromUtf8(e.what())); }
}
void Service::status(Entry& e, const QString& text) {
    e.status=text;
    if(window)window->scheduleThumbnailPreparation();
    if (e.card) {
        auto* card=static_cast<PreviewCard*>(e.card);
        card->setStatus(text,text + (e.log.isEmpty()?"":"\n\n"+e.log));
        card->updateTriangles(e.config,e.view?e.view->metrics():QJsonObject{});
        if (card->playback) card->setPlayback(e.config["settings"].toObject(),e.view?e.view->metrics():QJsonObject{},e.view?e.view->audioWaveform():nullptr);
        card->setAnimation(e.config["settings"].toObject(),e.view?e.view->metrics():QJsonObject{});
    }
}
QStringList Service::dependencies(const Entry& e) const {
    QStringList paths{e.config["path"].toString()};
    if(e.config["kind"]=="graph")paths.append(MaterialGraph::dependencies(e.config["path"].toString()));
    for (const auto& p:e.config["watch"].toArray()) paths.append(p.toString());
    for (const auto& p:e.config["maps"].toObject()) paths.append(p.toString());
    const auto inspection=e.config.value("inspection").toObject();
    if (inspection["enabled"].toBool() && !inspection["review"].toString().isEmpty()) paths.append(inspection["review"].toString());
    paths.append(e.dependencyMetadata.paths(e.config["path"].toString()));
    paths.removeDuplicates();
    return paths.mid(0,256);
}
PublicationSnapshot Service::outputStamps(const Entry& entry) const {
    QStringList paths{entry.config["path"].toString()};
    if(entry.config["kind"]=="graph")paths.append(MaterialGraph::dependencies(entry.config["path"].toString()));
    // Explicit watches can include imported dependencies that discovery misses.
    for (const auto& path:entry.config["watch"].toArray()) paths.append(path.toString());
    for (const auto& path:entry.config["maps"].toObject()) paths.append(path.toString());
    paths.append(entry.dependencyMetadata.paths(entry.config["path"].toString()));
    PublicationSnapshot result;
    for (const auto& path:paths) result.stamps[path]=stamp(path);
    const auto kind=entry.config["kind"].toString();
    result.complete=kind!="model" || entry.dependencyMetadata.complete();
    QStringList images;
    if (kind=="image" || kind=="material") images.append(entry.config["path"].toString());
    if (kind=="material") for (const auto& path:entry.config["maps"].toObject()) images.append(path.toString());
    if (kind=="model") images.append(entry.dependencyMetadata.imagePaths());
    const QSet<QString> currentImages(images.begin(),images.end());
    for (auto i=entry.imageCoverage.begin();i!=entry.imageCoverage.end();)
        if (!currentImages.contains(i.key())) i=entry.imageCoverage.erase(i); else ++i;
    if (!result.complete) { entry.imageCoverage.clear(); return result; }
    for (const auto& path:images) {
        const auto source=result.stamps.value(path);
        auto cached=entry.imageCoverage.constFind(path);
        if (cached==entry.imageCoverage.cend() || cached->first!=source) {
            // Detect actual contents: SVG can decode under a raster suffix, and
            // material readers can follow sidecars such as MHD's raw pixels.
            static const QSet<QByteArray> standalone{"png","jpeg","jpg","webp","bmp","tiff","tif","exr","hdr"};
            QImageReader reader(path);
            const bool complete=QFileInfo(path).isFile() && standalone.contains(reader.format().toLower());
            if (source!=stamp(path)) { entry.imageCoverage.remove(path); result.complete=false; continue; }
            entry.imageCoverage[path]={source,complete};
            cached=entry.imageCoverage.constFind(path);
        }
        result.complete &= cached->second;
    }
    return result;
}
bool Service::currentPublication(const Entry& entry) const {
    return entry.publicationValid && entry.view && !entry.view->loading() &&
        entry.view->metrics()["loads"].toInt()>0 && entry.publishedOutputs==outputStamps(entry);
}
void Service::recordPublication(Entry& entry) {
    if (entry.pendingOutputs.stamps.isEmpty() || !entry.view || entry.view->loading() || entry.view->metrics()["loads"].toInt()<1) return;
    const auto current=outputStamps(entry);
    entry.publicationValid=entry.pendingOutputs.complete && current.complete && entry.pendingOutputs==current &&
        current.stamps.value(entry.config["path"].toString())!="missing";
    // A last-displayed thumbnail may use a best-effort dependency manifest.
    // Import reuse still requires publicationValid and complete coverage.
    if (entry.pendingOutputs.stamps==current.stamps) entry.publishedOutputs=current;
    if (entry.publicationValid) {
        entry.publishedOutputs=current;
        // A retry or explicit reload can publish before the debounced watcher
        // handles the same save. Acknowledge the exact imported output stamps,
        // while leaving generator inputs for changed() to process normally.
        QSet<QString> sources;
        for (const auto& path:entry.config["watch"].toArray()) sources.insert(path.toString());
        for (auto i=current.stamps.begin();i!=current.stamps.end();++i)
            if (entry.stamps.contains(i.key()) && !sources.contains(i.key())) entry.stamps[i.key()]=i.value();
    }
    entry.pendingOutputs={};
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
    if (window && window->isVisible() && !window->isMinimized()) {
    // Warming watches registered source files/parents, never recursively scans.
    // Dependency watches belong only to visible or currently running jobs.
    for (const auto& id:order) {
        const auto path=entries[id]->config["path"].toString();
        if(QFileInfo(path).isFile())files.insert(path);
        auto parent=QFileInfo(path).absolutePath();
        while(!QFileInfo(parent).isDir() && parent!="/")parent=QFileInfo(parent).absolutePath();
        dirs.insert(parent);
    }
    auto watched=window->overviewIds(); watched.append(window->thumbnailWorkingIds()); watched.removeDuplicates();
    for (const auto& id:watched) {
        const auto e=entries.value(id); if (!e) continue;
        QStringList paths=e->thumbnailDependencies;
        paths.prepend(e->config["path"].toString());
        for (const auto& p:paths) {
            if (QFileInfo(p).isFile()) files.insert(p);
            QString parent=QFileInfo(p).absolutePath();
            while (!QFileInfo(parent).isDir() && parent!="/") parent=QFileInfo(parent).absolutePath();
            dirs.insert(parent);
        }
    }
    }
    for(const auto& path:preloader.targetPaths()) {
        if (QFileInfo(path).isFile()) files.insert(path);
        QString parent=QFileInfo(path).absolutePath();
        while (!QFileInfo(parent).isDir() && parent!="/") parent=QFileInfo(parent).absolutePath();
        dirs.insert(parent);
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
    if (window) window->refreshOverview();
    for (const auto& id:order) {
        auto e=entries[id]; if (!e->active) continue;
        bool dirty=false, source=false, output=false, reviewDirty=false;
        const auto review=e->config.value("inspection").toObject()["review"].toString();
        QSet<QString> sourcePaths;
        for (const auto& p:e->config["watch"].toArray()) sourcePaths.insert(p.toString());
        for (auto i=e->stamps.begin();i!=e->stamps.end();++i) {
            const auto next=stamp(i.key());
            if (next!=i.value()) {
                if (!review.isEmpty() && i.key()==review) { reviewDirty=true; i.value()=next; continue; }
                dirty=true; source|=sourcePaths.contains(i.key()); output|=!sourcePaths.contains(i.key()); i.value()=next;
            }
        }
        if (reviewDirty) { e->reviewStamp.clear(); inspectionEvent(id); }
        if (dirty) {
            ++e->token; e->retries=0;
            if (source && !e->config["command"].toArray().isEmpty()) build(id);
            if (output || !source || e->config["command"].toArray().isEmpty()) reload(id);
        }
    }
    preloadReturnTarget(); watchers();
}
void Service::reload(const QString& id) {
    auto e=entries.value(id); if (!e || !e->active || !e->view) return;
    QString error;
    if (!playbackKind(e->config["kind"].toString()) && QFileInfo(e->config["path"].toString()).size()>256*1024*1024) { status(*e,"File exceeds 256 MiB"); return; }
    const bool building=e->process && e->process->state()!=QProcess::NotRunning;
    e->publicationValid=false;
    e->pendingOutputs=outputStamps(*e);
    auto config=e->config;
    if (config.value("inspection").toObject()["enabled"].toBool()) config=inspectionConfig(*e);
    if (e->view->reload(config,error)) {
        recordPublication(*e);
        if (e->view->loading()) { status(*e,building?"Building · loading "+e->config["kind"].toString():"Loading · "+e->config["kind"].toString()); return; }
        ++e->revisions; e->retries=0; ++e->token;
        status(*e,(building?"Building · preview updated ":"Live · refreshed ") + QTime::currentTime().toString("HH:mm:ss"));
        if (inspectionId==id) inspectionEvent(id);
    }
    else {
        e->pendingOutputs={};
        status(*e,(building?"Building · waiting · ":"Waiting · ") + error);
        if (++e->retries<=3) {
            const auto token=e->token;
            QTimer::singleShot(200*(1<<e->retries),this,[this,id,token] {
                const auto e=entries.value(id); if (e && e->active && e->token==token && !e->status.startsWith("Generator failed") && !e->status.startsWith("Generator timed out")) reload(id);
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
        if (error==QProcess::FailedToStart) { e->forceGeneratorReload=false; e->deadline->stop(); status(*e,"Generator failed · " + e->process->errorString()); }
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
            const bool reuse=!e->forceGeneratorReload && currentPublication(*e);
            e->forceGeneratorReload=false;
            if (reuse) status(*e,"Live · generator complete");
            else reload(id);
            QSet<QString> sources;
            for (const auto& p:e->config["watch"].toArray()) sources.insert(p.toString());
            for (const auto& p:dependencies(*e)) if (!sources.contains(p)) e->stamps[p]=stamp(p);
            watchers();
        }
        else { e->forceGeneratorReload=false; status(*e,"Generator failed · exit " + QString::number(code)); }
    });
    connect(p,&QProcess::started,this,[this,id] {
        auto e=entries.value(id); if (!e || !e->process) return;
        e->process->setProperty("groupPid",e->process->processId());
        e->deadline->start(int(e->config["timeout"].toDouble(120)*1000));
    });
    connect(e->deadline.get(),&QTimer::timeout,this,[this,id] {
        auto e=entries.value(id); if (!e) return;
        e->forceGeneratorReload=false; stopBuilder(*e); status(*e,"Generator timed out");
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
    for (const auto& key:{"orthographic","up_axis","front_axis","lock_horizon"}) if (settings.contains(key)) targetSettings[key]=settings[key]; else targetSettings.remove(key);
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
    releasedViews=true;
    if (window && window->isVisible() && !window->isMinimized()) rememberThumbnail(*e);
    if (e->view && e->config.value("inspection").toObject()["enabled"].toBool()) {
        QString error; const auto state=e->view->inspect({{"action","state"}},error);
        if (error.isEmpty()) {
            for (const auto& key:{"cuts","hidden","selected","selection","revision","dependency_revision","orthographic"}) e->inspectionView[key]=state[key];
            e->inspectionView["camera"]=e->view->cameraState();
        }
    }
    e->active=false; ++e->token;
    e->publicationValid=false; e->pendingOutputs={}; e->publishedOutputs={}; e->forceGeneratorReload=false;
    e->dependencyMetadata.clear(); e->imageCoverage.clear();
    stopBuilder(*e);
    if (window) window->removeCard(*e);
    e->view=nullptr; e->surface=nullptr; e->statusLabel=nullptr; e->optionsButton=nullptr; e->card=nullptr; e->stamps.clear();
    status(*e,"Suspended");
    e->reviewDocument={}; e->reviewDigest.clear(); e->reviewStamp.clear(); e->markerDigest.clear(); e->markerRevision.clear();
}
QString Service::thumbnailKey(Entry& entry) {
    const auto path=entry.config["path"].toString();
    const auto source=preview::sourceCacheKey("overview-v1",path);
    if (source.isEmpty()) return {};
    // OBJ texture membership can change without touching the OBJ itself.
    // Refresh that bounded manifest before fingerprinting a background result.
    if(entry.thumbnailSource==source)for(auto it=entry.thumbnailManifests.cbegin();it!=entry.thumbnailManifests.cend();++it)
        if(stamp(it.key())!=it.value()) { entry.thumbnailSource.clear(); break; }
    if (entry.thumbnailSource!=source) {
        entry.thumbnailDependencies=dependencies(entry);
        entry.thumbnailSource=source;
        qsizetype bytes=0;
        for (const auto& p:entry.thumbnailDependencies) bytes+=p.toUtf8().size();
        entry.thumbnailUnavailable=entry.thumbnailDependencies.size()>64 || bytes>16*1024;
        if (entry.thumbnailUnavailable) entry.thumbnailDependencies.clear();
        entry.thumbnailManifests.clear();
        for(const auto& dependency:entry.thumbnailDependencies)
            if(dependency.endsWith(".mtl",Qt::CaseInsensitive))entry.thumbnailManifests[dependency]=stamp(dependency);
        if (!entry.active) { entry.dependencyMetadata.clear(); entry.imageCoverage.clear(); }
    }
    if (entry.thumbnailUnavailable) return {};
    QByteArray identity=source.toUtf8(); qsizetype pathBytes=0;
    if (entry.thumbnailDependencies.size()>64) return {};
    for (const auto& p:entry.thumbnailDependencies) {
        pathBytes+=p.toUtf8().size(); if (pathBytes>16*1024) return {};
        identity+='\0'+p.toUtf8()+'\0'+stamp(p).toUtf8();
    }
    QJsonObject visual;
    for (const auto& key:{"kind","settings","camera","image_view","maps","sample_model","inspection"}) visual[key]=entry.config.value(key);
    identity+=QJsonDocument(visual).toJson(QJsonDocument::Compact);
    const auto key=QString::fromLatin1(QCryptographicHash::hash(identity,QCryptographicHash::Sha256).toHex());
    cache.identify(key,"thumbnail",path);
    return key;
}
void Service::rememberThumbnail(Entry& entry) {
    if (!entry.view || !entry.surface || entry.view->loading()) return;
    const auto metrics=entry.view->metrics();
    if(metrics["loads"].toInt()<1 || metrics["command_pending"].toBool()) return;
    const auto current=outputStamps(entry);
    if (current.stamps!=entry.publishedOutputs.stamps || current.stamps.value(entry.config["path"].toString())=="missing") return;
    const auto key=thumbnailKey(entry); if (key.isEmpty()) return;
    QImage image;
    if (const auto* still=dynamic_cast<ImageView*>(entry.view)) {
        if (!cache.peekThumbnail(key).isNull()) return;
        image=still->thumbnail();
        if(image.isNull())return; // Overview can prepare a missing cache-hit thumbnail asynchronously.
    }
    else {
        const auto size=entry.surface->size()*entry.surface->devicePixelRatioF();
        if (qint64(size.width())*size.height()*4>16*1024*1024) return;
        image=entry.view->snapshot();
    }
    if (key==thumbnailKey(entry)) cache.putThumbnail(key,std::move(image));
}
QString Service::closeTarget(const QString& id) const {
    const int index=order.indexOf(id);
    if (index<0 || order.size()<2) return {};
    if (closeBehavior=="last")
        for (auto it=selectionHistory.crbegin();it!=selectionHistory.crend();++it)
            if (*it!=id && entries.contains(*it)) return *it;
    return order.value(index>0?index-1:1);
}
void Service::preloadReturnTarget(bool settled) {
    auto id=closeTarget(selected);
    if(layout!="projects") { projectPreloadDelay.stop(); pendingProjectPreload.clear(); }
    if(layout=="projects") {
        id.clear(); const auto current=entries.value(selected);
        if(current && current->active && !current->config.value("project").toString().isEmpty()) {
            QStringList members; const auto group=current->config.value("project").toString();
            for(const auto& key:order)if(entries[key]->config.value("project").toString()==group)members.append(key);
            const int at=members.indexOf(selected),next=at+projectDirection,other=at-projectDirection;
            if(next>=0 && next<members.size())id=members[next];
            else if(other>=0 && other<members.size())id=members[other];
        }
        for(const auto& entry:entries)if(entry->active && ((entry->view && entry->view->loading()) ||
            (entry->process && entry->process->state()!=QProcess::NotRunning)))id.clear();
    }
    const auto entry=entries.value(id);
    if (!window || !window->isVisible() || window->isMinimized() || layout=="overview" || !entry || entry->active) {
        projectPreloadDelay.stop(); pendingProjectPreload.clear();
        preloader.target(); return;
    }
    if(layout=="projects") {
        if(settled && pendingProjectPreload!=id)return;
        if(!settled && preloader.targetId()!=id) {
            if(pendingProjectPreload!=id) {
                preloader.target(); pendingProjectPreload=id; projectPreloadDelay.start();
            }
            return;
        }
    }
    preloader.target(id,entry->config["kind"].toString(),entry->config["path"].toString(),dependencies(*entry));
}
QString Service::projectCurrent(const QString& project) const {
    const auto current=projects.value(project).toObject()["current"].toString();
    if (order.contains(current) && entries.value(current) && entries[current]->config.value("project").toString()==project) return current;
    for (auto it=order.crbegin();it!=order.crend();++it)
        if (entries[*it]->config.value("project").toString()==project) return *it;
    return {};
}
QJsonArray Service::projectRows() const {
    QJsonArray result; QMap<QString,QJsonArray> groups;
    for(const auto& asset:order) {
        const auto& config=entries[asset]->config;
        groups[config.value("project").toString()].append(QJsonObject{{"id",asset},{"label",config["label"]}});
    }
    auto append=[&](const QString& id,const QString& label) {
        const auto members=groups.value(id);
        result.append(QJsonObject{{"id",id},{"label",label},{"current",projectCurrent(id)},{"entries",members}});
    };
    for (const auto& id:projectOrder) append(id,projects.value(id).toObject()["label"].toString());
    for (const auto& id:order) if (entries[id]->config.value("project").toString().isEmpty()) { append({},"Ungrouped"); break; }
    return result;
}
QStringList Service::overviewOrder() const {
    QStringList result;
    auto accepts=[&](const QString& id) {
        const auto project=entries[id]->config.value("project").toString();
        return projectFilterId.isEmpty() || (projectFilterId=="@ungrouped"?project.isEmpty():project==projectFilterId);
    };
    if (groupByProject) {
        for (const auto& project:projectRows()) for (const auto& item:project.toObject()["entries"].toArray()) {
            const auto id=item.toObject()["id"].toString(); if (accepts(id)) result.append(id);
        }
    } else for (const auto& id:order) if (accepts(id)) result.append(id);
    return result;
}
void Service::reconcile() {
    QElapsedTimer clock; clock.start(); qint64 previous=0; QJsonObject phases;
    auto phase=[&](const char* name) { const auto now=clock.nsecsElapsed(); phases[name]=(now-previous)/1e6; previous=now; };
    QStringList visible;
    const bool projectMode=layout=="projects" && inspectionId.isEmpty() && comparison.isEmpty() && window && !window->isolatesPreview();
    if (projectMode && window->isVisible() && !window->isMinimized()) visible=window->prepareProjects();
    if (window && window->isVisible() && !window->isMinimized() && !order.isEmpty()) {
        if (layout=="overview" && inspectionId.isEmpty() && comparison.isEmpty()) {}
        else if (window->isolatesPreview() && entries.contains(selected)) visible={selected};
        else if (!inspectionId.isEmpty() && entries.contains(inspectionId)) visible={inspectionId};
        else if (!comparison.isEmpty()) visible=comparison;
        else if (projectMode) {}
        else { int i=std::max(0,int(order.indexOf(selected)));
            const int count=pageSize(); visible=order.mid((i/count)*count,count); }
    }
    phase("prepare_ms");
    for (const auto& id:order) if (!visible.contains(id)) deactivate(id);
    phase("unload_ms");
    QElapsedTimer admission; admission.start(); int created=0; bool deferred=false;
    for (const auto& id:visible) {
        const auto entry=entries[id];
        if(!entry->active && created && (created>=2 || admission.elapsed()>=8)) { deferred=true; continue; }
        if (entry->active && entry->view) {
            const bool inspecting=inspectionId==id && entry->config.value("inspection").toObject()["enabled"].toBool();
            if ((entry->view->metrics()["backend"]=="vtk-gltf")!=inspecting) replaceInspectionView(id);
        }
        const bool fresh=!entry->active; activate(id); created+=fresh;
    }
    if(deferred && !activationScheduled) {
        activationScheduled=true;
        QTimer::singleShot(1,this,[this] { activationScheduled=false; reconcile(); });
    }
    phase("activate_ms");
    QStringList published; for(const auto& id:visible)if(entries[id]->active)published.append(id);
    if (window) { window->arrange(published); phase("arrange_ms"); window->navigation(); phase("navigation_ms"); window->refreshInspection(); }
    if (!window || !window->isVisible() || window->isMinimized()) cache.clear(false,"hidden");
    else cache.trimForPressure();
    preloadReturnTarget();
    if(window)window->prepareThumbnails();
    watchers();
#if defined(__GLIBC__)
    if (releasedViews && visible.isEmpty()) {
        QElapsedTimer timer; timer.start(); const bool released=::malloc_trim(0)!=0;
        cache.observe("heap","trim",{},0,"views_released",timer.nsecsElapsed()/1000,{{"released",released}});
    }
#endif
    phase("watch_cache_trim_ms"); phases["total_ms"]=clock.nsecsElapsed()/1e6; runtimeTiming["reconcile"]=phases;
    releasedViews=false;
}
QJsonObject Service::navigate(int delta) {
    if (layout=="projects") return success();
    if (!inspectionId.isEmpty()) return success();
    if (!comparison.isEmpty()) return success();
    const auto navigationOrder=layout=="overview"?overviewOrder():order;
    if (navigationOrder.isEmpty()) return success();
    int index=std::max(0,int(navigationOrder.indexOf(selected)));
    if (layout=="grid" && !(window && window->isolatesPreview())) {
        const int page=index/pageSize();
        const int target=std::clamp(page+delta,0,int(navigationOrder.size()-1)/pageSize());
        if (target==page) return success();
        index=target*pageSize();
    } else {
        const int target=std::clamp(index+delta,0,int(navigationOrder.size())-1);
        if (target==index) return success();
        index=target;
    }
    return request({{"method","select"},{"id",navigationOrder[index]}});
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
    copyAction=action(file,"&Copy Image / Frame","edit-copy",QKeySequence("Ctrl+Shift+C"),[this] { saveScreenshot(service->selected,true); });
    action(file,"Copy View Screenshot","edit-copy",{},[this] {
        const auto entry=service->entries.value(service->selected);
        if (entry && entry->view) QGuiApplication::clipboard()->setImage(entry->view->snapshot());
    });
    action(file,"Annotate / Reference View…","edit-comment",QKeySequence("Ctrl+Shift+A"),[this] { reference(); });
    action(file,"Annotate / Reference Window…","edit-comment",{},[this] { reference(true); });
    action(file,"Save Window Screenshot","document-save-as",{},[this] { saveScreenshot(service->selected,false,true); });
    action(file,"Open Screenshot Folder","folder-pictures",{},[this] {
        const auto directory=captureDirectory();
        if (!QDir().mkpath(directory)) { reportError("Cannot create screenshot folder: "+directory); return; }
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory))) reportError("Cannot open screenshot folder: "+directory);
    });
    file->addSeparator();
    closeAction=action(file,"&Close Preview","window-close",QKeySequence::Close,[this,command] { command({{"method","remove"},{"id",service->selected}}); });
    closeSelectedAction=action(file,"Close &Selected Previews","window-close",{},[this] { closeMarked(); });
    reopenAction=action(file,"&Reopen Closed Preview","edit-undo",QKeySequence("Ctrl+Shift+T"),[command] { command({{"method","reopen"}}); });
    auto* closeAllAction=action(file,"Close &All Previews","window-close",{},[command] { command({{"method","close_all"}}); });
    connect(file,&QMenu::aboutToShow,this,[this,closeAllAction] { closeAllAction->setEnabled(!service->order.isEmpty()); });
    file->addSeparator();
    action(file,"&Quit","application-exit",QKeySequence::Quit,[invoke] { invoke({{"method","quit"}}); });

    auto* view=menuBar()->addMenu("&View");
    action(view,"Project Previews","view-grid",QKeySequence("Ctrl+9"),[invoke] { invoke({{"method","layout"},{"layout","projects"}}); });
    action(view,"All Open Previews","view-grid",QKeySequence("Ctrl+0"),[invoke] { invoke({{"method","layout"},{"layout","overview"}}); });
    action(view,"Clear Image and Thumbnail Cache","edit-clear",{},[invoke] { invoke({{"method","cache"},{"action","clear"}}); });
    action(view,"Codex Sidebar","utilities-terminal",QKeySequence("Ctrl+Shift+K"),[this] {
        if (codexDock && codexDock->isVisible()) collapseCodex(); else openCodex();
    });
    fullscreenAction=action(view,"&Full Screen","view-fullscreen",QKeySequence("F11"),[this] { toggleFullscreen(); });
    fullscreenAction->setCheckable(true);
    view->addSeparator();
    inspectAction=action(view,"Inspect &3D","edit-find",{},[this,invoke] {
        invoke({{"method","inspect"},{"id",service->selected},{"action",service->inspectionId==service->selected?"close":"open"}});
    });
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
    for (int i=0;i<6;++i) {
        auto* item=layoutMenu->addAction(i==5?"Project Previews":i==4?"All Open Previews":i==0?"Single Preview":QString("%1 × %1 Grid").arg(i+1));
        item->setCheckable(true); item->setData(i); layoutGroup->addAction(item);
        connect(item,&QAction::triggered,this,[invoke,i] {
            QJsonObject patch{{"method","layout"},{"layout",i==5?"projects":i==4?"overview":i==0?"single":"grid"}};
            if (i>0 && i<4) patch["grid_size"]=i+1;
            invoke(patch);
        });
    }
    connect(layoutMenu,&QMenu::aboutToShow,this,[this,layoutGroup] {
        const int index=service->layout=="projects"?5:service->layout=="overview"?4:service->layout=="single"?0:service->gridSize-1;
        for (auto* item:layoutGroup->actions()) item->setChecked(service->comparison.isEmpty() && item->data().toInt()==index);
    });
    connect(view,&QMenu::aboutToShow,this,[this,current] { current->setEnabled(!service->selected.isEmpty()); });
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
        qDeleteAll(settings->findChildren<QMenu*>(QString(),Qt::FindDirectChildrenOnly));
        settings->clear();
        auto* themes=settings->addMenu("&Theme");
        auto* choices=new QActionGroup(themes);
        for (const auto& mode:QStringList{"auto","dark","light"}) {
            auto label=mode; label[0]=label[0].toUpper();
            auto* choice=themes->addAction(label);
            choice->setData(mode); choice->setCheckable(true); choice->setChecked(service->theme==mode);
            choices->addAction(choice);
            connect(choice,&QAction::triggered,this,[this,choices,mode] {
                const auto result=service->request({{"method","theme"},{"mode",mode}});
                if (!result["ok"].toBool()) {
                    for (auto* item:choices->actions()) item->setChecked(item->data().toString()==service->theme);
                    reportError(result["error"].toString());
                }
            });
        }
        auto* closing=settings->addMenu("After &Closing a Preview");
        auto* destinations=new QActionGroup(closing);
        destinations->setExclusive(true);
        for (const auto& mode:QStringList{"left","last"}) {
            auto* choice=closing->addAction(mode=="left"?"&Left Preview":"&Last Viewed Preview");
            choice->setData(mode); choice->setCheckable(true); choice->setChecked(service->closeBehavior==mode);
            destinations->addAction(choice);
            connect(choice,&QAction::triggered,this,[this,destinations,mode] {
                const auto result=service->request({{"method","close_behavior"},{"mode",mode}});
                if (!result["ok"].toBool()) {
                    for (auto* item:destinations->actions()) item->setChecked(item->data().toString()==service->closeBehavior);
                    reportError(result["error"].toString());
                }
            });
        }
        settings->addSeparator(); settings->addSection("Defaults for New Previews");
        for (const auto& group:QStringList{"image","video","audio","3d"}) {
            auto* defaults=settings->addMenu(group=="3d"?"&3D / Materials":group=="image"?"&Images":group=="audio"?"&Audio / Music":"&Videos");
            populateSettings(defaults,group=="3d"?"material":group,service->preferences.value(group).toObject(),[this,group](QJsonObject patch) {
                const auto result=service->request({{"method","defaults"},{"group",group},{"settings",patch}});
                if (!result["ok"].toBool()) reportError(result["error"].toString());
            },true);
        }
    });
    auto* help=menuBar()->addMenu("&Help");
    action(help,"&Keyboard Shortcuts","help-contents",QKeySequence::HelpContents,[this] {
        QMessageBox::information(this,"Keyboard Shortcuts","Point at a preview to target its shortcuts and menus.\nOutside previews, the last active preview stays the target.\n\nOpen assets: Ctrl+O\nSelect previews: checkbox / Ctrl+click\nSelect range: Shift+click · Add range: Ctrl+Shift+click\nClose checked previews: Close selected in toolbar / File menu\nOverview: Ctrl+Space toggles selection\nClose preview: Ctrl+W\nReopen closed preview: Ctrl+Shift+T\nPrevious / next: Left / Right\nFull screen: F11 · Exit: Esc or F11\nVideo full screen: double click\nFit view: F\nReset camera: Ctrl+Home\nEdges: E · Triangle count: M (3D)\nCamera: 1 front · 3 right · 7 top · 9 isometric\nCtrl+1 / 3 / 7: opposite view · Numpad supported\nSave screenshot: Ctrl+Shift+S\nCopy screenshot: Ctrl+Shift+C\nReload / rebuild: Ctrl+R\nAudio/video: Space pause · M mute · Home restart\nAudio/video seek: Shift+Left / Right\n3D animation: Space pause · Home restart · Shift+Left / Right seek");
    });
    action(help,"&About Asset Preview","help-about",{},[this] { QMessageBox::about(this,"About Asset Preview","Asset Preview\nLive images, audio, models, videos and baked materials.\nMIT licensed. Dependencies retain their own licenses."); });
}

PreviewWindow::PreviewWindow(Service* s) : service(s) {
    hoverPosition=QCursor::pos();
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
    density=new QComboBox; density->addItems({"Single","2×2","3×3","4×4","Overview","Projects"}); density->setMinimumHeight(32);
    density->setToolTip("Single / live grid / all open previews as lightweight thumbnails (Ctrl+0)");
    inspectionMode=new QComboBox; inspectionMode->setObjectName("inspectionMode");
    inspectionMode->addItem("Preview","preview"); inspectionMode->addItem("Part mode","parts"); inspectionMode->addItem("Cut mode","cuts");
    inspectionMode->setMinimumHeight(32); inspectionMode->setAccessibleName("3D preview mode");
    inspectionMode->setToolTip("Switch between the ordinary preview, component selection and planar cuts (static GLB/glTF)");
    connect(inspectionMode,&QComboBox::activated,this,&PreviewWindow::switchInspectionMode);
    syncButton=new QCheckBox("Sync camera"); syncButton->setToolTip("Link orbit, pan and zoom in both comparison views"); syncButton->hide();
    position=new QLabel; position->setMinimumWidth(65); position->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
    controls->addWidget(previous); controls->addWidget(selection,1); controls->addWidget(next); controls->addWidget(position);
    auto* codexButton=new QToolButton; codexButton->setText("Codex ›"); codexButton->setObjectName("codexToggle"); codexButton->setAutoRaise(true);
    codexButton->setToolTip("Open / collapse the originating Codex conversation (Ctrl+Shift+K)");
    connect(codexButton,&QToolButton::clicked,this,[this] { if(codexDock && codexDock->isVisible()) collapseCodex(); else openCodex(); });
    auto* referenceButton=new QToolButton; referenceButton->setText("Reference"); referenceButton->setObjectName("referenceView"); referenceButton->setAutoRaise(true);
    referenceButton->setToolTip("Capture what you are looking at, annotate it and attach it to Codex (Ctrl+Shift+A)");
    connect(referenceButton,&QToolButton::clicked,this,[this] { reference(); });
    controls->addWidget(referenceButton); controls->addWidget(codexButton);
    controls->addWidget(density); controls->addWidget(inspectionMode); controls->addWidget(syncButton); controls->addWidget(addButton); controls->addWidget(closeAllButton);
    auto* toolbar=new QToolBar("Preview Toolbar",this); toolbar->setObjectName("previewToolbar"); toolbar->setMovable(false);
    auto* toolbarControls=new QWidget; toolbarControls->setLayout(controls); controls->setContentsMargins(4,2,4,2);
    toolbarControls->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed); toolbar->addWidget(toolbarControls); addToolBar(toolbar);
    canvas=new QWidget; canvas->setFocusPolicy(Qt::StrongFocus);
    grid=new QGridLayout(canvas); grid->setContentsMargins(0,0,0,0); grid->setSpacing(6);
    projectTools=new QWidget(central); auto* projectBar=new QHBoxLayout(projectTools);
    projectBar->setContentsMargins(0,0,0,0); projectBar->setSpacing(8);
    groupProjects=new QCheckBox("Group by project",projectTools); groupProjects->setObjectName("groupProjects");
    projectFilter=new QComboBox(projectTools); projectFilter->setObjectName("projectFilter"); projectFilter->setAccessibleName("Filter by project");
    projectFilter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); projectFilter->setMinimumContentsLength(12);
    projectFilter->setMaximumWidth(280);
    projectDensity=new QComboBox(projectTools); projectDensity->setObjectName("projectDensity"); projectDensity->setAccessibleName("Project grid size");
    projectDensity->addItems({"Auto grid","2×2","3×3","4×4"});
    projectBar->addWidget(groupProjects); projectBar->addWidget(projectFilter); projectBar->addStretch(); projectBar->addWidget(projectDensity);
    vertical->addWidget(projectTools);
    auto updateProjectLayout=[this](QJsonObject request) {
        request["method"]="layout"; const auto result=service->request(request);
        if (!result["ok"].toBool()) { reportError(result["error"].toString()); navigation(); }
    };
    connect(groupProjects,&QCheckBox::toggled,this,[updateProjectLayout](bool enabled) { updateProjectLayout({{"group_projects",enabled}}); });
    connect(projectFilter,&QComboBox::activated,this,[this,updateProjectLayout](int i) { updateProjectLayout({{"project_filter",projectFilter->itemData(i).toString()}}); });
    connect(projectDensity,&QComboBox::activated,this,[updateProjectLayout](int i) { updateProjectLayout({{"project_grid_size",i==0?0:i+1}}); });
    projectGrid=new ProjectGrid(canvas); projectGrid->hide();
    projectGrid->viewportChanged=[this] { service->reconcile(); };
    projectGrid->chosen=[this](const QString& project,const QString& current) {
        if (project.isEmpty()) { service->request({{"method","zoom"},{"id",current}}); return; }
        const auto result=service->request({{"method","project"},{"id",project},{"current",current}});
        if (!result["ok"].toBool()) reportError(result["error"].toString());
    };
    overview=new OverviewGrid(&service->cache,service->thumbnailDirectory,canvas); overview->hide();
    overview->keyFor=[this](const QString& id) { const auto e=service->entries.value(id); return e?service->thumbnailKey(*e):QString(); };
    overview->configFor=[this](const QString& id) {
        QJsonObject config;
        if (const auto entry=service->entries.value(id))
            for (const auto* name:{"path","kind","settings","camera","maps","sample_model"})
                if (entry->config.contains(name)) config[name]=entry->config[name];
        return config;
    };
    overview->activated=[this](const QString& id) {
        markAnchor=id;
        const auto result=service->request({{"method","zoom"},{"id",id}});
        if (!result["ok"].toBool()) reportError(result["error"].toString());
        else if (const auto e=service->entries.value(id);e && e->surface) e->surface->setFocus(Qt::OtherFocusReason);
    };
    overview->selected=[this](const QString& id) { selectCard(id); };
    overview->marked=[this](const QString& id,Qt::KeyboardModifiers modifiers) { markCard(id,modifiers); };
    overview->viewportChanged=[this] {
        if(thumbnailWatchScheduled)return;
        thumbnailWatchScheduled=true;
        QTimer::singleShot(0,this,[this] { thumbnailWatchScheduled=false; service->watchers(); });
    };
    vertical->addWidget(canvas,1); setCentralWidget(central);
    statusBar()->showMessage("Drag files to add · Left / Right to switch · close to suspend");
    connect(statusBar(),&QStatusBar::messageChanged,this,[this](const QString& message) {
        if (message.isEmpty()) statusBar()->setVisible(!isFullScreen());
    });
    connect(addButton,&QToolButton::clicked,this,[this] {
        QJsonArray paths;
        for (const auto& path:QFileDialog::getOpenFileNames(this,"Add asset previews",QDir::homePath(),"Assets (*.ptex *.materialgraph.json *.png *.jpg *.jpeg *.webp *.bmp *.svg *.glb *.gltf *.obj *.stl *.ply *.fbx *.post *.vxz *.ovoxel *.mp4 *.m4v *.mov *.mkv *.webm *.avi *.ogv *.mpg *.mpeg *.wmv *.gif *.wav *.wave *.mp3 *.flac *.ogg *.oga *.opus *.m4a *.aac *.aiff *.aif *.wma *.ape);;All files (*)"))
            paths.append(path);
        if (paths.isEmpty()) return;
        const auto result=service->request({{"method","open"},{"paths",paths}});
        if (!result["ok"].toBool()) reportError(result["error"].toString());
    });
    connect(closeAllButton,&QToolButton::clicked,this,[this] {
        if (!marked.isEmpty()) { closeMarked(); return; }
        const auto result=service->request({{"method","close_all"}});
        if (!result["ok"].toBool()) reportError(result["error"].toString());
    });
    connect(density,&QComboBox::activated,this,[this](int index) {
        QJsonObject request{{"method","layout"},{"layout",index==5?"projects":index==4?"overview":index==0?"single":"grid"}};
        if (index>0 && index<4) request["grid_size"]=index+1;
        const auto result=service->request(request);
        if (!result["ok"].toBool()) reportError(result["error"].toString());
    });
    connect(syncButton,&QCheckBox::toggled,this,[this](bool value) { service->request({{"method","compare"},{"camera_sync",value}}); });
    auto navigate=[this](int delta) {
        if (projectGrid->isVisible()) {
            const auto target=projectGrid->adjacentHovered(delta);
            if (target.isEmpty()) return;
            // Unload the hovered native surface only after moving focus away.
            canvas->setFocus(Qt::OtherFocusReason);
            const int previousDirection=service->projectDirection; service->projectDirection=delta;
            const auto result=service->request({{"method","select"},{"id",target},{"make_current",true}});
            if(!result["ok"].toBool())service->projectDirection=previousDirection;
            const auto entry=service->entries.value(service->selected);
            if (entry && entry->surface) entry->surface->setFocus(Qt::OtherFocusReason);
            if (!result["ok"].toBool()) reportError(result["error"].toString());
            return;
        }
        if (overviewVisible()) {
            const auto result=service->navigate(delta); overview->select(service->selected,true);
            if (!result["ok"].toBool()) reportError(result["error"].toString());
            return;
        }
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
    exitFullscreen=new QShortcut(QKeySequence("Esc"),this); exitFullscreen->setEnabled(false); exitFullscreen->setAutoRepeat(false);
    connect(exitFullscreen,&QShortcut::activated,this,&PreviewWindow::leaveFullscreen);
    fullscreenNotice=new FullscreenNotice(canvas);
    fullscreenNotice->exitRequested=[this] { leaveFullscreen(); };
    connect(qApp,&QApplication::focusChanged,this,[this](QWidget*,QWidget*) {
        const auto context=terminalFocus()?Qt::WidgetShortcut:Qt::WindowShortcut;
        for (auto* action:actions()) action->setShortcutContext(context);
        for (auto* shortcut:findChildren<QShortcut*>(QString(),Qt::FindDirectChildrenOnly)) shortcut->setContext(context);
    });
    QSettings settings(service->statePath+"/window.ini",QSettings::IniFormat);
    const auto fraction=settings.value("codex_fraction",7.0/16.0).toDouble();
    if (std::isfinite(fraction) && fraction>=.15 && fraction<=.85) codexFraction=fraction;
    restoreGeometry(settings.value("geometry").toByteArray());
}
PreviewWindow::~PreviewWindow() {
    QSettings settings(service->statePath+"/window.ini",QSettings::IniFormat);
    settings.setValue("codex_fraction",codexFraction);
}
void PreviewWindow::toggleFullscreen(bool focusPreview) {
    if (isFullScreen()) { leaveFullscreen(); return; }
    fullscreenFocus=focusPreview;
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowFullScreen);
}
void PreviewWindow::leaveFullscreen() {
    if (!isFullScreen()) return;
    setWindowState(windowedState);
    if (!(windowedState & Qt::WindowMaximized) && windowedGeometry.isValid()) setGeometry(windowedGeometry);
}
void PreviewWindow::fullscreenChanged() {
    const bool full=isFullScreen();
    fullscreenMode=full;
    if (!full) fullscreenFocus=false;
    fullscreenAction->setChecked(full); exitFullscreen->setEnabled(full);
    menuBar()->setVisible(!full);
    findChild<QToolBar*>("previewToolbar")->setVisible(!full);
    statusBar()->setVisible(!full);
    for (const auto& entry:service->entries) if (entry->card) {
        auto* card=static_cast<PreviewCard*>(entry->card);
        card->setFullscreen(full);
        card->activity();
    }
    if (full && isVisible()) fullscreenNotice->announce();
    else fullscreenNotice->dismiss();
}
QJsonObject PreviewWindow::presentation() const {
    const auto modePosition=inspectionMode->mapTo(const_cast<PreviewWindow*>(this),QPoint());
    QStringList markedIds;
    for (const auto& id:service->order) if (marked.contains(id)) markedIds.append(id);
    const auto closePosition=closeAllButton->mapTo(const_cast<PreviewWindow*>(this),QPoint());
    return {{"fullscreen",isFullScreen()},{"single_preview",isolatesPreview()},
        {"marked",QJsonArray::fromStringList(markedIds)},
        {"close_selected",QJsonObject{{"x",closePosition.x()},{"y",closePosition.y()},{"width",closeAllButton->width()},
            {"height",closeAllButton->height()},{"visible",closeAllButton->isVisible() && !marked.isEmpty()},{"enabled",!marked.isEmpty()}}},
        {"width",width()},{"height",height()},{"menu_visible",menuBar()->isVisible()},{"overview",overview->describe(const_cast<PreviewWindow*>(this))},
        {"projects",projectGrid->describe(const_cast<PreviewWindow*>(this))},
        {"toolbar_visible",findChild<QToolBar*>("previewToolbar")->isVisible()},
        {"inspection_mode",inspectionMode->currentData().toString()},
        {"mode_control",QJsonObject{{"x",modePosition.x()},{"y",modePosition.y()},{"width",inspectionMode->width()},
            {"height",inspectionMode->height()},{"visible",inspectionMode->isVisible()},{"enabled",inspectionMode->isEnabled()}}},
        {"codex",codexPanel?codexPanel->describe(const_cast<PreviewWindow*>(this)):QJsonObject{}},
        {"codex_fraction",codexFraction},
        {"reference_controls",referenceControls()},
        {"notice",fullscreenNotice->describe(const_cast<PreviewWindow*>(this))}};
}
QJsonObject PreviewWindow::referenceControls() const {
    QJsonObject controls;
    for (const auto& name:{"codexToggle","referenceView","referenceWindow","attachPng","codexReconnect","codexCollapse"}) {
        if (auto* widget=findChild<QWidget*>(name)) {
            auto at=widget->mapTo(const_cast<PreviewWindow*>(this),QPoint());
            controls[name]=QJsonObject{{"x",at.x()},{"y",at.y()},{"width",widget->width()},{"height",widget->height()},{"visible",widget->isVisible()}};
        }
    }
    if (auto* editor=findChild<QDialog*>("referenceEditor")) {
        QJsonObject widgets;
        for (auto* widget:editor->findChildren<QWidget*>()) {
            QString name=widget->objectName();
            if (auto* button=qobject_cast<QAbstractButton*>(widget)) name=button->text();
            if (name.isEmpty() || !widget->isVisible()) continue;
            auto at=widget->mapToGlobal(QPoint())-mapToGlobal(QPoint());
            widgets[name]=QJsonObject{{"x",at.x()},{"y",at.y()},{"width",widget->width()},{"height",widget->height()}};
        }
        controls["editor"]=widgets;
    }
    return controls;
}
void PreviewWindow::composeNotice(QPainter& painter) { fullscreenNotice->compose(painter,this); }
void PreviewWindow::present() {
    // Repeated GUI requests must not undo maximized/fullscreen or desktop placement.
    const bool wasVisible=isVisible();
    if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    if (!wasVisible && isFullScreen()) fullscreenNotice->announce();
    const auto keepOnScreen=[this] {
        if (isMaximized() || isFullScreen()) return;
        QList<QRect> screens;
        if (auto* primary=QGuiApplication::primaryScreen()) screens.append(primary->availableGeometry());
        for (auto* screen:QGuiApplication::screens())
            if (screen!=QGuiApplication::primaryScreen()) screens.append(screen->availableGeometry());
        const QRect frame=frameGeometry();
        const QRect fitted=fitWindowFrame(frame,screens);
        if (fitted==frame) return;
        const QSize decoration=frame.size()-size();
        resize(fitted.size()-decoration);
        move(pos()+fitted.topLeft()-frameGeometry().topLeft());
    };
    keepOnScreen();
    // X11 frame decorations become authoritative after the window is mapped.
    QTimer::singleShot(0,this,keepOnScreen);
    raise(); activateWindow();
}
void PreviewWindow::navigation() {
    if (hoverTarget!=service->selected) {
        hoverTarget=service->selected;
        hoverPosition=QCursor::pos();
    }
    QSignalBlocker block(selection);
    bool choicesChanged=selection->count()!=service->order.size();
    if(!choicesChanged)for(int at=0;at<service->order.size();++at) {
        const auto id=service->order[at];
        if(selection->itemData(at)!=id || selection->itemText(at)!=service->entries[id]->config["label"].toString()) { choicesChanged=true; break; }
    }
    if(choicesChanged) {
        selection->clear();
        for (const auto& id:service->order) selection->addItem(service->entries[id]->config["label"].toString(),id);
    }
    selection->setCurrentIndex(service->order.indexOf(service->selected));
    const int i=std::max(0,int(service->order.indexOf(service->selected))), n=service->order.size();
    const int count=service->pageSize();
    previous->setEnabled(n>0 && i/count>0);
    next->setEnabled(n>0 && (i/count+1)*count<n);
    closeAllButton->setEnabled(n>0);
    refreshMarked();
    closeAction->setEnabled(n>0); reopenAction->setEnabled(!service->closedEntries.isEmpty() && service->inspectionId.isEmpty());
    const auto selected=service->entries.value(service->selected);
    const bool ready=selected && selected->active && selected->view && selected->view->metrics()["loads"].toInt()>0;
    saveAction->setEnabled(ready);
    copyAction->setEnabled(saveAction->isEnabled());
    const bool model=ready && (selected->config["kind"]=="model" || selected->config["kind"]=="material");
    fitAction->setEnabled(ready && !playbackKind(selected->config["kind"].toString()));
    for (auto* item:modelActions) item->setEnabled(model);
    for (auto* item:modelShortcuts) item->setEnabled(model);
    for (const auto& id:service->order) if (service->entries[id]->card)
        static_cast<PreviewCard*>(service->entries[id]->card)->setSelected(id==service->selected);
    const bool projectLayout=service->layout=="projects", overviewLayout=service->layout=="overview";
    projectTools->setVisible((projectLayout || overviewLayout) && service->inspectionId.isEmpty() && service->comparison.isEmpty() && !isolatesPreview());
    groupProjects->setVisible(overviewLayout); projectDensity->setVisible(projectLayout);
    { QSignalBlocker b(groupProjects); groupProjects->setChecked(service->groupByProject); }
    { QSignalBlocker b(projectDensity); projectDensity->setCurrentIndex(service->projectGridSize==0?0:service->projectGridSize-1); }
    { QSignalBlocker b(projectFilter);
      bool changed=projectFilter->count()!=service->projectOrder.size()+2;
      if(!changed)for(int at=0;at<service->projectOrder.size();++at) {
          const auto id=service->projectOrder[at];
          if(projectFilter->itemData(at+1)!=id || projectFilter->itemText(at+1)!=service->projects.value(id).toObject()["label"].toString()) { changed=true; break; }
      }
      if(changed) {
          projectFilter->clear(); projectFilter->addItem("All projects",QString());
          for (const auto& id:service->projectOrder) projectFilter->addItem(projectIcon(id),service->projects.value(id).toObject()["label"].toString(),id);
          projectFilter->addItem("Ungrouped","@ungrouped");
      }
      projectFilter->setCurrentIndex(projectFilter->findData(service->projectFilterId)); }
    QSignalBlocker densityBlock(density);
    density->setCurrentIndex(service->layout=="projects"?5:service->layout=="overview"?4:service->layout=="single"?0:service->gridSize-1);
    density->setEnabled(service->comparison.isEmpty());
    if (inspectAction) {
        inspectAction->setEnabled(selected && selected->config["kind"]=="model");
        inspectAction->setText(service->inspectionId.isEmpty()?"Inspect &3D":"Return to &Preview");
    }
    {
        const bool model=selected && selected->config["kind"]=="model";
        const auto suffix=model?QFileInfo(selected->config["path"].toString()).suffix().toLower():QString();
        const bool supported=model && (suffix=="glb" || suffix=="gltf") && ASSET_PREVIEW_HAS_INSPECTION &&
            QFileInfo::exists(QCoreApplication::applicationDirPath()+"/asset-preview-inspection.so");
        const bool inspected=model && service->inspectionId==service->selected;
        const QSignalBlocker blocker(inspectionMode);
        inspectionMode->setVisible(model && !overviewVisible());
        inspectionMode->setEnabled(supported || inspected);
        auto* choices=qobject_cast<QStandardItemModel*>(inspectionMode->model());
        choices->item(1)->setEnabled(supported); choices->item(2)->setEnabled(supported);
        inspectionMode->setToolTip(supported?"Preview: ordinary view · Part mode: select, hide and isolate · Cut mode: X/Y/Z sections":
            "Part and cut modes require a static GLB/glTF model and the optional inspection backend");
        inspectionMode->setCurrentIndex(inspected?(selected->inspectionTool=="cuts"?2:1):0);
    }
    { QSignalBlocker blocker(syncButton); syncButton->setVisible(!service->comparison.isEmpty()); syncButton->setChecked(service->cameraSync); }
    if (projectLayout && service->inspectionId.isEmpty() && service->comparison.isEmpty() && !isolatesPreview()) {
        previous->setEnabled(false); next->setEnabled(false); position->setText(QString("%1 projects").arg(projectGrid->count())); return;
    }
    if (overviewVisible()) {
        previous->setEnabled(false); next->setEnabled(false); position->setText(QString("%1 open").arg(service->overviewOrder().size()));
        overview->select(service->selected); return;
    }
    if (!service->comparison.isEmpty()) { previous->setEnabled(false); next->setEnabled(false); position->setText("Compare"); return; }
    if (!service->inspectionId.isEmpty()) {
        previous->setEnabled(false); next->setEnabled(false); density->setEnabled(false);
        position->setText("Inspect 3D"); return;
    }
    position->setText(n==0?"0 / 0":service->layout=="single"?QString("%1 / %2").arg(i+1).arg(n):QString("%1–%2 / %3").arg((i/count)*count+1).arg(std::min((i/count+1)*count,n)).arg(n));
}
void PreviewWindow::switchInspectionMode(int index) {
    const auto entry=service->entries.value(service->selected);
    if (!entry || entry->config["kind"]!="model") return;
    if (!commitInspectionControls()) { navigation(); return; }
    const auto previous=entry->inspectionTool;
    if (index>0) entry->inspectionTool=index==2?"cuts":"parts";
    if (index==0 || service->inspectionId!=service->selected) {
        const auto result=service->request({{"method","inspect"},{"id",service->selected},{"action",index==0?"close":"open"}});
        if (!result["ok"].toBool()) { entry->inspectionTool=previous; reportError(result["error"].toString()); }
    }
    refreshInspection(); navigation();
}
QStringList PreviewWindow::prepareProjects() {
    QJsonArray rows;
    for (const auto& value:service->projectRows()) {
        const auto id=value.toObject()["id"].toString();
        if (service->projectFilterId.isEmpty() || (service->projectFilterId=="@ungrouped"?id.isEmpty():id==service->projectFilterId)) rows.append(value);
    }
    projectGrid->show(); projectGrid->setProjects(rows,service->projectGridSize);
    return projectGrid->visibleIds();
}
void PreviewWindow::arrange(const QStringList& ids) {
    while (auto* item=grid->takeAt(0)) { if (item->widget() && item->widget()->property("empty").toBool()) delete item->widget(); delete item; }
    for (int i=0;i<4;++i) { grid->setRowStretch(i,0); grid->setColumnStretch(i,0); }
    const int inset=isFullScreen()?0:8, verticalInset=isFullScreen()?0:6;
    centralWidget()->layout()->setContentsMargins(inset,verticalInset,inset,verticalInset);
    centralWidget()->layout()->setSpacing(6);
    grid->setSpacing(6); statusBar()->setVisible(!isFullScreen());
    const bool showProjects=service->layout=="projects" && service->inspectionId.isEmpty() && service->comparison.isEmpty() && !isolatesPreview();
    projectGrid->setVisible(showProjects);
    const bool showOverview=service->layout=="overview" && service->inspectionId.isEmpty() && service->comparison.isEmpty();
    overview->setVisible(showOverview);
    if (showOverview) {
        QList<OverviewGrid::Item> items;
        for (const auto& id:service->overviewOrder()) {
            const auto& c=service->entries[id]->config; const auto project=c.value("project").toString();
            items.append({id,c["label"].toString(),c["path"].toString(),c["kind"].toString(),project,
                project.isEmpty()?"Ungrouped":service->projects.value(project).toObject()["label"].toString()});
        }
        overview->setItems(std::move(items),service->selected,service->groupByProject);
        grid->addWidget(overview,0,0); grid->setRowStretch(0,1); grid->setColumnStretch(0,1); overview->setFocus(Qt::OtherFocusReason);
    } else if (showProjects) {
        grid->addWidget(projectGrid,0,0); grid->setRowStretch(0,1); grid->setColumnStretch(0,1);
        for (const auto& id:ids) {
            auto* card=static_cast<PreviewCard*>(service->entries[id]->card); card->setFullscreen(isFullScreen()); projectGrid->attach(id,card);
        }
    } else if (ids.isEmpty()) {
        auto* empty=new QLabel("Watch your work take shape\n\nAdd or drop an image, audio track, video, model, or exported material.\nPreviews refresh when the files are saved.");
        empty->setProperty("empty",true); empty->setAlignment(Qt::AlignCenter); empty->setWordWrap(true);
        grid->addWidget(empty,0,0); grid->setRowStretch(0,1); grid->setColumnStretch(0,1);
    } else {
        for (int i=0;i<ids.size();++i) { const int cols=isolatesPreview()?1:!service->comparison.isEmpty()?2:service->layout=="grid"?std::min(service->gridSize,int(ids.size())):1;
            auto* card=service->entries[ids[i]]->card;
            if (card->parentWidget()!=canvas) card->setParent(canvas);
            card->show();
            static_cast<PreviewCard*>(card)->setFullscreen(isFullScreen());
            grid->addWidget(service->entries[ids[i]]->card,i/cols,i%cols); grid->setRowStretch(i/cols,1); grid->setColumnStretch(i%cols,1); }
    }
    if (fullscreenNotice->isVisible()) fullscreenNotice->raise();
}
void PreviewWindow::createCard(Entry& e) {
    const QString id=e.config["id"].toString();
    e.publicationValid=false; e.publishedOutputs={};
    e.pendingOutputs=service->outputStamps(e);
    const auto kind=e.config["kind"].toString();
    const bool native=playbackKind(kind) || (service->inspectionId==id && e.config.value("inspection").toObject()["enabled"].toBool());
    QWidget* parent=canvas;
    if(service->layout=="projects" && service->inspectionId.isEmpty() && service->comparison.isEmpty() && !isolatesPreview())
        if(auto* pane=projectGrid->cardParent(id))parent=pane;
    auto* card=new PreviewCard(parent,e.config["label"].toString(),e.config["path"].toString(),kind,native);
    card->model=e.config["kind"].toString()=="model" || e.config["kind"].toString()=="material";
    e.card=card; e.statusLabel=card->footer; e.optionsButton=card->options;
    service->status(e,e.status);
    QWidget* widget=nullptr;
    if (e.config["kind"].toString()=="image") {
        auto* image=new ImageView(e.card); e.view=image; widget=image;
        image->loaded=[this,id](bool ok,QString error) {
            const auto entry=service->entries.value(id); if (!entry || !entry->active) return;
            if (ok) {
                ++entry->token; entry->retries=0; ++entry->revisions; service->recordPublication(*entry);
            } else {
                entry->publicationValid=false; entry->pendingOutputs={};
                if (++entry->retries<=3) {
                    const auto token=entry->token;
                    QTimer::singleShot(200*(1<<entry->retries),service,[this,id,token] {
                        const auto e=service->entries.value(id); if (e && e->active && e->token==token && !e->status.startsWith("Generator failed") && !e->status.startsWith("Generator timed out")) service->reload(id);
                    });
                }
            }
            const bool building=entry->process && entry->process->state()!=QProcess::NotRunning;
            if(!entry->status.startsWith("Generator failed") && !entry->status.startsWith("Generator timed out")) service->status(*entry,ok?(building?"Building · preview updated":"Live · image ready"):(building?"Building · waiting · ":"Waiting · ")+error);
            if(id==service->selected)navigation();
            if (service->inspectionId==id) service->inspectionEvent(id);
        };
    }
    else if(e.config["kind"]=="graph") { auto* graph=new MaterialGraphView(e.card); e.view=graph; widget=graph; }
    else {
        const auto kind=e.config["kind"].toString();
        const bool media=playbackKind(kind);
        const bool asynchronous=media || !(service->inspectionId==id && e.config.value("inspection").toObject()["enabled"].toBool());
        auto loaded=[this,id,media,asynchronous,kind](bool ok,QString error) {
            const auto e=service->entries.value(id); if (!e || !e->active) return;
            if (ok) { ++e->token; e->retries=0; service->recordPublication(*e); }
            else { e->publicationValid=false; e->pendingOutputs={}; }
            if (asynchronous) {
                if (ok) { ++e->revisions; e->retries=0; }
                else if (++e->retries<=3) {
                    const auto token=e->token;
                    QTimer::singleShot(200*(1<<e->retries),service,[this,id,token] {
                        const auto e=service->entries.value(id); if (e && e->active && e->token==token && !e->status.startsWith("Generator failed") && !e->status.startsWith("Generator timed out")) service->reload(id);
                    });
                }
            }
            if (e->process && e->process->state()!=QProcess::NotRunning) { service->status(*e,e->status); return; }
            if(!e->status.startsWith("Generator failed") && !e->status.startsWith("Generator timed out")) service->status(*e,ok?(media?"Live · "+kind+" ready":"Live · model ready"):"Waiting · "+error);
            navigation();
            if (service->inspectionId==id) service->inspectionEvent(id);
            if (ok && !media && !service->comparison.isEmpty() && !service->syncingCamera) {
                service->syncCamera(service->selected);
                try { service->save(); } catch (const std::exception& error) { reportError(QString::fromUtf8(error.what())); }
            }
        };
        try {
            ModelHandle handle{};
            auto cameraChanged=[this,id](const QJsonObject& state,bool persist) { service->inspectionChanged(id,state,persist); };
            if (media) handle=createMedia(e.config,e.card,std::move(loaded),[this,id](QJsonObject patch) {
                service->request({{"method","settings"},{"id",id},{"settings",patch}});
            });
            else if (service->inspectionId==id && e.config.value("inspection").toObject()["enabled"].toBool()) {
                const auto config=inspectionConfig(e);
                handle=createInspection(config,e.card,std::move(loaded),cameraChanged,[this,id](const QJsonObject&) { service->inspectionEvent(id); });
            } else handle=createModel(e.config,e.card,std::move(loaded),cameraChanged);
            e.view=handle.view; widget=handle.surface;
        } catch (const std::exception& error) {
            e.status=QString(media?"Playback backend unavailable · ":"3D backend unavailable · ")+QString::fromUtf8(error.what());
            qWarning("%s",qPrintable(e.status));
            service->status(e,e.status);
            auto* message=new QLabel("Build the preview backends with tools/build.sh, then reopen this preview.",e.card);
            message->setWordWrap(true); message->setAlignment(Qt::AlignCenter); widget=message;
        }
    }
    e.surface=widget;
    widget->setAcceptDrops(true);
    if (e.view) {
        e.view->cache=&service->cache;
        e.view->loadingChanged=[this,id] {
            const auto entry=service->entries.value(id);
            if(entry && entry->active && entry->view && entry->view->loading()) service->status(*entry,"Loading · "+entry->config["kind"].toString());
        };
        e.view->activated=[this,id] { selectCard(id); };
        e.view->inspectionChanged=[this,id](const QJsonObject& state,bool persist) { service->inspectionChanged(id,state,persist); };
        if (card->playback || card->animation) e.view->playbackChanged=[this,id] {
            const auto entry=service->entries.value(id);
            if (entry && entry->active && entry->card && entry->view) {
                auto* target=static_cast<PreviewCard*>(entry->card);
                const auto metrics=entry->view->metrics(),settings=entry->config["settings"].toObject();
                if (target->playback) target->setPlayback(settings,metrics,entry->view->audioWaveform());
                target->setAnimation(settings,metrics);
            }
        };
        e.view->playbackSettingsRequested=[this,id](QJsonObject settings) {
            const auto result=service->request({{"method","settings"},{"id",id},{"settings",settings}});
            if (!result["ok"].toBool()) reportError(result["error"].toString());
        };
    }
    if (card->animation) {
        card->animation->stepRequested=[this,id](int direction) {
            const auto result=service->request({{"method","animation_step"},{"id",id},{"direction",direction}});
            if(!result["ok"].toBool())reportError(result["error"].toString());
        };
        card->animation->settingsRequested=[this,id](QJsonObject settings) {
            const auto result=service->request({{"method","settings"},{"id",id},{"settings",settings}});
            if (!result["ok"].toBool()) {
                reportError(result["error"].toString());
                const auto entry=service->entries.value(id); if (entry) service->status(*entry,entry->status);
            }
        };
        card->animation->seekRequested=[this,id](double seconds) {
            const auto result=service->request({{"method","seek"},{"id",id},{"seconds",seconds}});
            if (!result["ok"].toBool()) reportError(result["error"].toString());
        };
    }
    if (card->playback) {
        card->playback->setFullscreen(isFullScreen());
        card->playback->fullscreenRequested=[this,id] { selectCard(id); toggleFullscreen(true); };
        card->playback->settingsRequested=[this,id](QJsonObject settings) {
            const auto result=service->request({{"method","settings"},{"id",id},{"settings",settings}});
            if (!result["ok"].toBool()) {
                reportError(result["error"].toString());
                const auto entry=service->entries.value(id); if (entry) service->status(*entry,entry->status);
            }
        };
        card->playback->seekRequested=[this,id](double seconds) {
            const auto result=service->request({{"method","seek"},{"id",id},{"seconds",seconds}});
            if (!result["ok"].toBool()) reportError(result["error"].toString());
        };
    }
    for (auto* object:card->findChildren<QWidget*>()) {
        object->setMouseTracking(true);
        object->setProperty("previewId",id);
        if (card->playback && card->playback->isAncestorOf(object))
            if (auto* video=dynamic_cast<QObject*>(e.view)) object->installEventFilter(video);
        object->installEventFilter(this);
    }
    // Embedded OpenGL windows receive their own enter/move events, bypassing
    // the widget container. Route those through the same hover target path.
    if (auto* object=dynamic_cast<QObject*>(e.view)) {
        object->setProperty("previewId",id); object->installEventFilter(this);
    }
    card->setMouseTracking(true);
    card->setProperty("previewId",id); card->installEventFilter(this);
    card->setSurface(widget,qobject_cast<QWindow*>(dynamic_cast<QObject*>(e.view)));
    widget->setToolTip(e.config["kind"]=="video"?"Space to pause · double click for full screen · F11 full screen · Esc exit · Shift+Left / Right seek · Home restart · M mute":
        e.config["kind"]=="audio"?"Space / double click to pause · Shift+Left / Right seek · Home restart · M mute":
        e.config["kind"].toString()=="image"?"Scroll to zoom · drag to pan · double click to fit":
        e.config["kind"]=="graph"?"Click a node to inspect · double click or + to expand · Ctrl+scroll to zoom":"Drag to orbit · right / middle drag to pan · scroll to zoom · double click to fit · E edges / lines · M triangle count");
    connect(card->fit,&QToolButton::clicked,this,[this,id] {
        const auto e=service->entries.value(id); if (!e || !e->view) return;
        e->view->fit();
    });
    connect(card->options,&QToolButton::clicked,this,[this,id] { options(id); });
    connect(card->reset,&QToolButton::clicked,this,[this,id] {
        const auto entry=service->entries.value(id); if (entry && entry->view) entry->view->resetView();
    });
    connect(card->remove,&QToolButton::clicked,this,[this,id] { service->request({{"method","remove"},{"id",id}}); });
    card->selectBox->setChecked(marked.contains(id));
    connect(card->selectBox,&QCheckBox::clicked,this,[this,id] { markCard(id,QGuiApplication::keyboardModifiers()); });
}
void PreviewWindow::removeCard(Entry& e) {
    auto* card=std::exchange(e.card,nullptr);
    if (!card) return;
    // Native child destruction can synchronously send synthetic mouse/focus
    // events. Detach the published UI before any child can be destroyed.
    e.view=nullptr; e.surface=nullptr; e.statusLabel=nullptr; e.optionsButton=nullptr;
    grid->removeWidget(card); delete card;
}
bool PreviewWindow::overviewVisible() const { return overview && overview->isVisible() && !isMinimized(); }
QStringList PreviewWindow::overviewIds() const { return overviewVisible()?overview->visibleIds():QStringList{}; }
void PreviewWindow::refreshOverview(bool reset) {
    if (!overview) return;
    auto ids=overviewIds(); ids.append(thumbnailWorkingIds()); ids.removeDuplicates();
    for (const auto& id:ids) if (const auto e=service->entries.value(id)) {
        e->thumbnailSource.clear(); service->thumbnailKey(*e);
    }
    if (reset) overview->invalidate(); else overview->refresh();
}
QStringList PreviewWindow::thumbnailWorkingIds() const { return overview?overview->workingIds():QStringList{}; }
void PreviewWindow::scheduleThumbnailPreparation() {
    if(thumbnailPrepareScheduled)return;
    thumbnailPrepareScheduled=true;
    QTimer::singleShot(0,this,[this] {
        thumbnailPrepareScheduled=false; prepareThumbnails();
        if(service->layout=="projects") {
            if(overview->backgroundAllowed())service->preloadReturnTarget();
            else service->preloader.target();
        }
    });
}
void PreviewWindow::prepareThumbnails() {
    if(!overview)return;
    const bool enabled=isVisible() && !isMinimized();
    bool ready=enabled && !service->activationScheduled;
    QSet<QString> active; QStringList nearby;
    QList<OverviewGrid::Item> all;
    for(const auto& id:service->order) {
        const auto entry=service->entries.value(id); const auto& config=entry->config;
        all.append({id,config["label"].toString(),config["path"].toString(),config["kind"].toString(),{}, {}});
        if(!entry->active)continue;
        active.insert(id);
        ready &= !entry->view || !entry->view->loading();
        ready &= !entry->process || entry->process->state()==QProcess::NotRunning;
        const auto group=config["project"].toString(); QStringList members;
        for(const auto& member:service->order)if(service->entries[member]->config.value("project").toString()==group)members.append(member);
        const int at=members.indexOf(id);
        if(at+1<members.size())nearby.append(members[at+1]);
        if(at>0)nearby.append(members[at-1]);
    }
    overview->warmItems(std::move(all),std::move(nearby),std::move(active),ready,enabled);
}
void PreviewWindow::selectCard(const QString& id) {
    if (id==service->selected || !service->entries.contains(id)) return;
    const auto result=service->request({{"method","select"},{"id",id}});
    if (!result["ok"].toBool()) reportError(result["error"].toString());
}
void PreviewWindow::markCard(const QString& id,Qt::KeyboardModifiers modifiers) {
    if (!service->entries.contains(id)) return;
    QStringList rangeOrder=overviewVisible()?service->overviewOrder():service->order;
    if (projectGrid->isVisible()) {
        rangeOrder.clear();
        for (const auto& value:projectGrid->describe(this)["panes"].toArray()) {
            const auto current=value.toObject()["current"].toString(); if (!current.isEmpty()) rangeOrder.append(current);
        }
    }
    if (modifiers & Qt::ShiftModifier) {
        int anchor=rangeOrder.indexOf(markAnchor);
        if (anchor<0) { markAnchor=service->selected; anchor=rangeOrder.indexOf(markAnchor); }
        const int at=rangeOrder.indexOf(id);
        if (anchor<0) { markAnchor=id; anchor=at; }
        if (!(modifiers & Qt::ControlModifier)) marked.clear();
        for (int i=std::min(anchor,at);i<=std::max(anchor,at);++i) marked.insert(rangeOrder[i]);
    } else {
        if (marked.contains(id)) marked.remove(id); else marked.insert(id);
        markAnchor=id;
    }
    refreshMarked();
}
void PreviewWindow::refreshMarked() {
    for (auto it=marked.begin();it!=marked.end();) {
        if (!service->entries.contains(*it)) it=marked.erase(it); else ++it;
    }
    if (!service->entries.contains(markAnchor)) markAnchor.clear();
    for (const auto& id:service->order) if (const auto& entry=service->entries[id];entry->card) {
        auto* box=static_cast<PreviewCard*>(entry->card)->selectBox;
        const QSignalBlocker blocker(box); box->setChecked(marked.contains(id));
    }
    overview->setMarked(marked);
    closeSelectedAction->setEnabled(!marked.isEmpty());
    closeAllButton->setText(marked.isEmpty()?"Close All":width()<780?QString("Close (%1)").arg(marked.size()):QString("Close selected (%1)").arg(marked.size()));
    closeAllButton->setToolTip(marked.isEmpty()?"Close all previews and stop their generators; keep files and remembered settings":
        "Close selected previews and stop their generators; keep files and remembered settings");
    closeAllButton->setVisible(width()>=780 || !marked.isEmpty());
}
void PreviewWindow::closeMarked() {
    QJsonArray ids;
    for (const auto& id:service->order) if (marked.contains(id)) ids.append(id);
    if (ids.isEmpty()) return;
    const auto result=service->request({{"method","remove_many"},{"ids",ids}});
    if (!result["ok"].toBool()) reportError(result["error"].toString());
}
void PreviewWindow::hoverCard(const QString& id,const QPoint& position,bool explicitInput) {
    if (!isActiveWindow() || QApplication::activePopupWidget() || QApplication::activeModalWidget() ||
        QGuiApplication::mouseButtons()!=Qt::NoButton) return;
    const auto entry=service->entries.value(id);
    if (!entry || !entry->active || !entry->surface || !entry->surface->isVisible()) return;
    // Layout/mask changes can send Enter without pointer movement. Retain
    // explicit targeting and keyboard focus inside this card in that case.
    if (!explicitInput && position==hoverPosition) {
        if (id!=service->selected) return;
        auto* focused=QApplication::focusWidget();
        if (focused && focused!=entry->surface && entry->card->isAncestorOf(focused)) return;
    }
    selectCard(id);
    hoverPosition=position;
    // Playback keys are handled by the native video surface. Move keyboard
    // focus as well as the menu target so a previously focused video cannot
    // consume another preview's M shortcut.
    if (service->selected==id) entry->surface->setFocus(Qt::MouseFocusReason);
}
bool PreviewWindow::eventFilter(QObject* object,QEvent* event) {
    if (object==codexDock && event->type()==QEvent::Resize && !resizingCodex && !codexResizePending &&
        codexDock->isVisible() && (QGuiApplication::mouseButtons() & Qt::LeftButton)) {
        // Only an explicit splitter drag updates the preference. Window
        // resizing applies the same fraction and cannot overwrite it.
        codexFraction=std::clamp(double(codexDock->width())/std::max(1,width()),.15,.85);
    }
    const auto id=object->property("previewId").toString();
    if (!id.isEmpty()) {
        const auto entry=service->entries.value(id);
        if (!entry || !entry->active || !entry->card) return QMainWindow::eventFilter(object,event);
        if (event->type()==QEvent::DragEnter || event->type()==QEvent::DragMove) {
            acceptFileDrag(static_cast<QDragMoveEvent*>(event)); return true;
        }
        if (event->type()==QEvent::Drop) {
            dropEvent(static_cast<QDropEvent*>(event));
            return true;
        }
        static_cast<PreviewCard*>(entry->card)->inputActivity(object,event);
        const bool animated=event->type()==QEvent::KeyPress && entry->config["kind"]=="model" && entry->view &&
            !entry->view->metrics()["animations"].toArray().isEmpty();
        const bool nativeInput=object==entry->surface || object==dynamic_cast<QObject*>(entry->view);
        if ((event->type()==QEvent::MouseButtonPress || event->type()==QEvent::MouseButtonDblClick) && (nativeInput || object==entry->card)) {
            const auto* mouse=static_cast<QMouseEvent*>(event);
            if (mouse->button()==Qt::LeftButton && (mouse->modifiers() & (Qt::ControlModifier|Qt::ShiftModifier))) {
                markCard(id,mouse->modifiers()); return true;
            }
        }
        auto* animationBar=static_cast<PreviewCard*>(entry->card)->animation;
        auto* slider=qobject_cast<QSlider*>(object);
        const bool animationTimeline=animated && slider && animationBar && animationBar->isAncestorOf(slider);
        if (animated && (nativeInput || animationTimeline)) {
            auto* key=static_cast<QKeyEvent*>(event);
            auto* bar=animationBar;
            auto* focused=QApplication::focusWidget();
            if (nativeInput && focused && bar->isAncestorOf(focused) && focused->isVisible()) {
                QCoreApplication::sendEvent(focused,event); return true;
            }
            const auto modifiers=key->modifiers();
            if (nativeInput && (key->key()==Qt::Key_Tab || key->key()==Qt::Key_Backtab) &&
                !(modifiers & (Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))) {
                bar->focusPlayback(key->key()==Qt::Key_Backtab || bool(modifiers & Qt::ShiftModifier)); return true;
            }
            if ((key->key()==Qt::Key_Space || key->key()==Qt::Key_Home) && modifiers==Qt::NoModifier) {
                if (!key->isAutoRepeat()) {
                    if (key->key()==Qt::Key_Space) {
                        const bool paused=entry->view->metrics()["paused"].toBool(true);
                        service->request({{"method","settings"},{"id",id},{"settings",QJsonObject{{"paused",!paused}}}});
                    } else service->request({{"method","seek"},{"id",id},{"seconds",0}});
                }
                return true;
            }
            if ((key->key()==Qt::Key_Comma || key->key()==Qt::Key_Period) && modifiers==Qt::NoModifier) {
                service->request({{"method","animation_step"},{"id",id},{"direction",key->key()==Qt::Key_Comma?-1:1}}); return true;
            }
            if ((key->key()==Qt::Key_Left || key->key()==Qt::Key_Right) && modifiers==Qt::ShiftModifier) {
                const double at=entry->view->metrics()["position"].toDouble()+(key->key()==Qt::Key_Left?-5:5);
                service->request({{"method","seek"},{"id",id},{"seconds",std::max(0.0,at)}}); return true;
            }
        }
        if (entry && entry->config["kind"]=="video" && event->type()==QEvent::KeyPress &&
            (object==entry->surface || object==dynamic_cast<QObject*>(entry->view))) {
            // A native video repaint can receive keys while Qt retains focus
            // on a playback widget. Keep its native keyboard behavior intact.
            auto* focused=QApplication::focusWidget();
            auto* playback=static_cast<PreviewCard*>(entry->card)->playback;
            if (focused && playback->isAncestorOf(focused) && focused->isVisible()) {
                QCoreApplication::sendEvent(focused,event); return true;
            }
            const auto* key=static_cast<QKeyEvent*>(event);
            if ((key->key()==Qt::Key_Tab || key->key()==Qt::Key_Backtab) &&
                !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
                static_cast<PreviewCard*>(entry->card)->playback->focusPlayback(
                    key->key()==Qt::Key_Backtab || bool(key->modifiers() & Qt::ShiftModifier));
                event->accept(); return true;
            }
        }
        if (entry && entry->config["kind"]=="video" && event->type()==QEvent::MouseButtonDblClick &&
            (object==entry->surface || object==dynamic_cast<QObject*>(entry->view)) &&
            static_cast<QMouseEvent*>(event)->button()==Qt::LeftButton) {
            selectCard(id); toggleFullscreen(true); event->accept(); return true;
        }
        if (event->type()==QEvent::Enter) hoverCard(id,static_cast<QEnterEvent*>(event)->globalPosition().toPoint());
        else if (event->type()==QEvent::MouseMove) hoverCard(id,static_cast<QMouseEvent*>(event)->globalPosition().toPoint());
        else if (event->type()==QEvent::Wheel) hoverCard(id,static_cast<QWheelEvent*>(event)->globalPosition().toPoint(),true);
        else if (event->type()==QEvent::MouseButtonPress) {
            const auto* mouse=static_cast<QMouseEvent*>(event);
            hoverPosition=mouse->globalPosition().toPoint();
            if (mouse->button()==Qt::LeftButton && (nativeInput || object==entry->card)) markAnchor=id;
            selectCard(id);
            // Native activation alone can retain QWidget focus on a playback
            // control, especially when the pointer has not moved since Tab.
            if (nativeInput) entry->surface->setFocus(Qt::MouseFocusReason);
        }
    }
    if (!id.isEmpty() && event->type()==QEvent::ContextMenu) {
        auto* context=static_cast<QContextMenuEvent*>(event); selectCard(id);
        QMenu menu(this); populatePreviewMenu(&menu,id,true); menu.exec(context->globalPos());
        const auto entry=service->entries.value(id); if (entry && entry->card) static_cast<PreviewCard*>(entry->card)->activity();
        return true;
    }
    return QMainWindow::eventFilter(object,event);
}
void PreviewWindow::reportError(const QString& error) {
    statusBar()->setVisible(true);
    statusBar()->showMessage(error,5000);
}
bool PreviewWindow::terminalFocus() const { return codexPanel && codexPanel->ownsFocus(); }
void PreviewWindow::openCodex() {
    if (!codexDock) {
        codexDock=new QDockWidget("Codex · originating conversation",this); codexDock->setObjectName("codexDock");
        codexDock->setAllowedAreas(Qt::RightDockWidgetArea); codexDock->setFeatures(QDockWidget::DockWidgetClosable);
        auto* content=new QWidget; auto* layout=new QVBoxLayout(content); layout->setContentsMargins(4,4,4,4);
        auto* actions=new QHBoxLayout;
        auto button=[&](const QString& label,const QString& name,const QString& tip,const std::function<void()>& run) {
            auto* b=new QToolButton; b->setText(label); b->setObjectName(name); b->setToolTip(tip); b->setAutoRaise(true);
            actions->addWidget(b); connect(b,&QToolButton::clicked,this,run);
        };
        button("View","dockReferenceView","Annotate and attach the current view",[this] { reference(); });
        button("Window","referenceWindow","Annotate the actual Preview interface",[this] { reference(true); });
        button("PNG…","attachPng","Attach another image to the draft",[this] {
            const auto path=QFileDialog::getOpenFileName(this,"Attach reference image",screenshotDirectory(),"Images (*.png *.jpg *.jpeg *.webp)");
            if (!path.isEmpty() && !codexPanel->pasteImage(path)) reportError("Wait for the Codex composer before attaching an image.");
        });
        button("Follow asset","codexReconnect","Reconnect to the selected asset's originating conversation",[this] {
            const auto entry=service->entries.value(service->selected);
            codexPanel->attach(entry && entry->config.contains("codex")?entry->config["codex"].toObject():service->codexOrigin);
        });
        actions->addStretch();
        button("›","codexCollapse","Collapse right; keep conversation, draft and background work",[this] { collapseCodex(); });
        layout->addLayout(actions); codexPanel=new CodexPanel; layout->addWidget(codexPanel,1);
        codexDock->setWidget(content); codexDock->installEventFilter(this);
        addDockWidget(Qt::RightDockWidgetArea,codexDock);
        codexDock->show(); resizeCodex();
    }
    codexDock->show();
    // An open terminal stays pinned while hovering/navigating other assets.
    if (!codexPanel->describe(this)["connected"].toBool()) {
        const auto entry=service->entries.value(service->selected);
        codexPanel->attach(entry && entry->config.contains("codex")?entry->config["codex"].toObject():service->codexOrigin);
    }
}
void PreviewWindow::collapseCodex() { if (codexDock) codexDock->hide(); canvas->setFocus(); }
void PreviewWindow::resizeCodex() {
    if (!codexDock || !codexDock->isVisible()) return;
    resizingCodex=true;
    resizeDocks({codexDock},{qRound(width()*codexFraction)},Qt::Horizontal);
    resizingCodex=false;
}
QImage PreviewWindow::captureWindow() {
    if (auto* editor=findChild<QDialog*>("referenceEditor"); editor && editor->isVisible()) return editor->grab().toImage();
    if (auto* modal=QApplication::activeModalWidget(); modal && isAncestorOf(modal)) return modal->grab().toImage();
    auto image=grab().toImage(); QPainter painter(&image);
    if (projectGrid->isVisible()) painter.setClipRect(QRect(projectGrid->viewport()->mapTo(this,QPoint()),projectGrid->viewport()->size()));
    for (const auto& entry:service->entries) if (entry->active && entry->view && entry->config["kind"]!="image")
        painter.drawImage(QRect(entry->surface->mapTo(this,QPoint()),entry->surface->size()),entry->view->snapshot());
    for (const auto& entry:service->entries) if (entry->active && entry->card) static_cast<PreviewCard*>(entry->card)->compose(painter,this);
    painter.setClipping(false); composeNotice(painter); return image;
}
void PreviewWindow::reference(bool wholeWindow) {
    const auto entry=service->entries.value(service->selected);
    if (!entry || !entry->active || !entry->view) { reportError("Open a preview before capturing a reference."); return; }
    auto image=wholeWindow?captureWindow():entry->view->snapshot();
    if (image.isNull()) { reportError("This preview has no frame available yet."); return; }
    // Capture identity/state at the same moment as the frozen pixels.
    const auto metrics=entry->view->metrics();
    QString context="Preview reference: "+service->selected+"\nAsset: "+entry->config["path"].toString();
    if (metrics.contains("position")) context+="\nFrame time: "+QString::number(metrics["position"].toDouble(),'f',3)+" s";
    const auto state=entry->view->inspectionState();
    if (!state.isEmpty()) context+="\nView: "+QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact));
    editReference(std::move(image),wholeWindow?"window":service->selected,context,[this](const QString& path,const QString& detail) {
        openCodex(); return codexPanel->pasteImage(path,detail);
    },this);
}
void PreviewWindow::saveScreenshot(const QString& id,bool clipboard,bool wholeWindow) {
    const auto entry=service->entries.value(id);
    if (!wholeWindow && (!entry || !entry->view || !entry->active)) { reportError("Open a preview before taking a screenshot."); return; }
    QImage capture=wholeWindow?captureWindow():entry->view->snapshot();
    if (!wholeWindow) if (auto* image=dynamic_cast<ImageView*>(entry->view)) capture=image->pixels();
    if (capture.isNull()) { reportError("This preview is not ready for a screenshot."); return; }
    if (clipboard) { QGuiApplication::clipboard()->setImage(capture); statusBar()->showMessage("Image copied to clipboard",4000); return; }
    QString error; const auto path=saveReferencePng(capture,wholeWindow?"window":id,error);
    if (path.isEmpty()) reportError(error); else statusBar()->showMessage("Screenshot saved: "+path,7000);
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
    action(isFullScreen()?"Exit &Full Screen\tEsc / F11":"&Full Screen\tF11","view-fullscreen",
        [this,kind] { toggleFullscreen(kind=="video"); });
    if (!playbackKind(kind)) action("&Fit View\tF","zoom-fit-best",[entry] { if (entry->view) entry->view->fit(); });
    if (kind=="model" || kind=="material") {
        action("&Reset Camera\tCtrl+Home","view-refresh",[entry] { if (entry->view) entry->view->resetView(); });
        cameraViews(menu->addMenu("Camera &Views"),id);
    }
    if (kind=="model") action(service->inspectionId==id?"Return to Preview":"Inspect 3D","edit-find",[this,id] {
        const auto result=service->request({{"method","inspect"},{"id",id},{"action",service->inspectionId==id?"close":"open"}});
        if (!result["ok"].toBool()) reportError(result["error"].toString());
    });
    menu->addSeparator();
    auto patch=[this,id](QJsonObject fields) {
        const auto result=service->request({{"method","settings"},{"id",id},{"settings",fields}});
        if (!result["ok"].toBool()) reportError(result["error"].toString());
    };
    const bool inspector=service->inspectionId==id && entry->config.value("inspection").toObject()["enabled"].toBool();
    populateSettings(menu,inspector?QString("inspection"):kind,entry->config["settings"].toObject(),patch);
    const auto suffix=QFileInfo(entry->config["path"].toString()).suffix().toLower();
    if(kind=="model" && !inspector && (suffix=="vxz" || suffix=="ovoxel")) {
        auto* voxelMenu=menu->addMenu("O-Voxel View");
        const auto selected=entry->config["settings"].toObject()["ovoxel_mode"].toString("surface");
        auto* choices=new QActionGroup(voxelMenu);choices->setExclusive(true);
        const QList<std::pair<QString,QString>> modes{{"surface","Extracted raw surface"},{"cells","Raw voxel cells"},{"voxels","Occupied cell centers"},{"vertices","Decoded vertex positions"}};
        for(const auto& [mode,label]:modes) {
            auto* item=voxelMenu->addAction(label);item->setCheckable(true);item->setChecked(mode==selected);choices->addAction(item);
            item->setToolTip(mode=="cells"?"Exact occupied grid cells; checker shading identifies individual cells":
                mode=="voxels"?"Cell centers; marker diameter is 0.6 cell and scales with zoom":
                mode=="vertices"?"Decoded sub-cell positions; marker diameter is 0.36 cell and scales with zoom":
                "Surface extracted from the stored geometry; no filling or remeshing");
            connect(item,&QAction::triggered,this,[patch,mode]{patch({{"ovoxel_mode",mode}});});
        }
    }
    if (kind=="model" && !inspector && entry->view) {
        const auto metrics=entry->view->metrics(),settings=entry->config["settings"].toObject();
        const auto clips=metrics["animations"].toArray();
        if (!clips.isEmpty()) {
            menu->addSection("Animation");
            action(metrics["paused"].toBool(true)?"Play Animation\tSpace":"Pause Animation\tSpace","media-playback-start",
                [patch,metrics] { patch({{"paused",!metrics["paused"].toBool(true)}}); });
            auto* clipMenu=menu->addMenu("Animation Clip");
            for (int i=0;i<clips.size();++i) {
                auto* item=clipMenu->addAction(clips[i].toString()); item->setCheckable(true); item->setChecked(metrics["animation"].toInt()==i);
                connect(item,&QAction::triggered,this,[patch,i] { patch({{"animation",i}}); });
            }
            auto* loop=menu->addAction("Loop Animation"); loop->setCheckable(true); loop->setChecked(settings["loop"].toBool(true));
            connect(loop,&QAction::triggered,this,[patch](bool value) { patch({{"loop",value}}); });
            auto* stepped=menu->addAction("Stepped Playback"); stepped->setCheckable(true);
            stepped->setChecked(settings["animation_stepped"].toBool());
            connect(stepped,&QAction::triggered,this,[patch](bool value) { patch({{"animation_stepped",value}}); });
            auto* frameRate=menu->addMenu("Animation Frame Rate");
            for (int value:{6,8,12,15,24,30,60,120}) {
                auto* item=frameRate->addAction(QString::number(value)+" fps"); item->setCheckable(true);
                item->setChecked(settings["animation_fps"].toInt(12)==value);
                connect(item,&QAction::triggered,this,[patch,value] { patch({{"animation_fps",value}}); });
            }
            for (int direction:{-1,1}) action(direction<0?"Previous Frame\t,":"Next Frame\t.","media-seek-forward",[this,id,direction] {
                const auto result=service->request({{"method","animation_step"},{"id",id},{"direction",direction}});
                if(!result["ok"].toBool())reportError(result["error"].toString());
            });
            auto* speed=menu->addMenu("Animation Speed");
            for (double value:{.25,.5,1.,2.,4.}) {
                auto* item=speed->addAction(QString::number(value)+"×"); item->setCheckable(true);
                item->setChecked(settings["animation_speed"].toDouble(1)==value);
                connect(item,&QAction::triggered,this,[patch,value] { patch({{"animation_speed",value}}); });
            }
        }
    }
    if (playbackKind(kind) || (kind=="model" && !inspector && entry->view && !entry->view->metrics()["animations"].toArray().isEmpty())) {
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
        action("Copy Image / Frame\tCtrl+Shift+C","edit-copy",[this,id] { saveScreenshot(id,true); });
        action("Annotate / Reference View…","edit-comment",[this] { reference(); });
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
    const auto current=service->entries.value(id); if (current && current->card) static_cast<PreviewCard*>(current->card)->activity();
}
void PreviewWindow::populateSettings(QMenu* menu,const QString& kind,const QJsonObject& settings,
    const std::function<void(QJsonObject)>& patch,bool defaults) {
    if(kind=="graph") {menu->addSection("Recipe layout and saved-map previews");return;}
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
    else if (playbackKind(kind)) {
        toggle("&Paused\tSpace","paused",kind=="audio"); toggle("&Muted\tM","muted",kind!="audio"); toggle("&Loop","loop",true);

    }
    else {
        menu->addSection("Display");
        toggle("Show Triangle &Count\tM","triangles",true);
        toggle("Show &Materials","materials",true);
        toggle("Show &Textures","textures",true)->setEnabled(settings["materials"].toBool(true));
        toggle("Lock &horizon","lock_horizon",true);
        if (kind!="inspection") { toggle("Ground Grid","grid",true); toggle("Axis Indicator","axes"); }
        toggle("Show Edges / Lines\tE","edges"); toggle("Orthographic Camera","orthographic");
        choice("Floor / up axis","up_axis",{"Y up · XZ floor","Z up · XY floor"},{"y","z"},"y");
        if (kind!="inspection") {
            if(kind=="model" || defaults)
                choice("Model texture detail","texture_quality",{"Original","Fast GLB preview · 1024 px"},{"original","preview"},"original");
            choice("Lighting","lighting",{"Studio · HDRI + light kit","F3D light kit"},{"studio","lightkit"},"studio");
            number("Light intensity","light",1,5);
        } else menu->addSection("Authored lighting / inspection light kit");
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
    const bool dockVisible=codexDock && codexDock->isVisible();
    if (dockVisible) codexResizePending=true;
    QMainWindow::resizeEvent(e);
    if (!selection) return;
    refreshMarked();
    position->setVisible(width()>=700);
    syncButton->setText(width()<760?"Sync":"Sync camera");
    if (dockVisible) QTimer::singleShot(0,this,[this] { resizeCodex(); codexResizePending=false; });
}
void PreviewWindow::closeEvent(QCloseEvent* event) {
    leaveFullscreen();
    QSettings settings(service->statePath+"/window.ini",QSettings::IniFormat); settings.setValue("geometry",saveGeometry());
    settings.setValue("codex_fraction",codexFraction);
    for (const auto& entry:service->entries) if (entry->active) service->rememberThumbnail(*entry);
    hide(); service->reconcile(); event->ignore();
}
void PreviewWindow::changeEvent(QEvent* e) {
    QMainWindow::changeEvent(e);
    if (e->type()==QEvent::WindowStateChange) {
        if (overview) overview->invalidate();
        if (isFullScreen()!=fullscreenMode) {
            if (isFullScreen()) {
                windowedState=static_cast<QWindowStateChangeEvent*>(e)->oldState() & ~(Qt::WindowFullScreen | Qt::WindowMinimized);
                windowedGeometry=normalGeometry();
            }
            fullscreenChanged();
        }
        QTimer::singleShot(0,service,&Service::reconcile);
    }
}
void PreviewWindow::dragEnterEvent(QDragEnterEvent* e) {
    acceptFileDrag(e);
}
void PreviewWindow::dropEvent(QDropEvent* e) {
    if (!acceptFileDrag(e)) return;
    const auto paths=droppedFiles(e->mimeData());
    // Switching selection may destroy the native drop receiver. Finish its Qt
    // event first, then open the copied paths through the window's lifetime.
    QTimer::singleShot(0,this,[this,paths] {
        const auto result=service->request({{"method","open"},{"paths",paths}});
        if (!result["ok"].toBool()) reportError(result["error"].toString());
    });
}
