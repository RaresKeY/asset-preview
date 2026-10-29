#pragma once
#include "viewer.h"
#include "dependency_metadata.h"
#include "preview_cache.h"
#include "preview_preload.h"
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QLocalServer>
#include <QMainWindow>
#include <QMap>
#include <QProcess>
#include <QPoint>
#include <QSet>
#include <QTimer>
#include <memory>
#include <utility>

class QMenu;
class QAction;
class QShortcut;
class QLabel;
class QComboBox;
class QGridLayout;
class QFrame;
class QCheckBox;
class QToolButton;
class QDockWidget;
class InspectionPanel;
class FullscreenNotice;
class CodexPanel;
class Service;
class OverviewGrid;
class ProjectGrid;

struct PublicationSnapshot {
    QMap<QString,QString> stamps;
    bool complete=false;
    bool operator==(const PublicationSnapshot&) const = default;
};

struct Entry {
    QJsonObject config;
    QString status = "Suspended", log;
    QMap<QString, QString> stamps;
    mutable preview::DependencyMetadata dependencyMetadata;
    PublicationSnapshot pendingOutputs, publishedOutputs;
    mutable QMap<QString,std::pair<QString,bool>> imageCoverage;
    bool publicationValid = false, forceGeneratorReload = false;
    PreviewView* view = nullptr;
    QWidget* surface = nullptr;
    QFrame* card = nullptr;
    QLabel* statusLabel = nullptr;
    QToolButton* optionsButton = nullptr;
    std::unique_ptr<QProcess> process;
    std::unique_ptr<QTimer> deadline;
    bool active = false, rerun = false;
    int builds = 0, revisions = 0, retries = 0;
    quint64 token = 0;
    QJsonObject inspectionView, reviewDocument;
    QString inspectionTool = "parts";
    QString reviewDigest, reviewStamp, markerRevision, markerDigest;
    qint64 reviewRevision = 0;
    bool reviewEstablished = false;
    bool inspectionNotificationPending = false;
    QString thumbnailSource;
    QStringList thumbnailDependencies;
    QMap<QString,QString> thumbnailManifests;
    bool thumbnailUnavailable=false;
};

class PreviewWindow final : public QMainWindow {
public:
    explicit PreviewWindow(Service* service);
    ~PreviewWindow() override;
    void present();
    void navigation();
    void arrange(const QStringList& ids);
    void createCard(Entry& entry);
    void removeCard(Entry& entry);
    void reportError(const QString& error);
    void refreshInspection();
    bool commitInspectionControls();
    bool isolatesPreview() const { return isFullScreen() && fullscreenFocus; }
    QJsonObject presentation() const;
    QJsonObject referenceControls() const;
    void composeNotice(QPainter&);
    void openCodex();
    void collapseCodex();
    bool terminalFocus() const;
    void reference(bool wholeWindow=false);
    QImage captureWindow();
    bool overviewVisible() const;
    QStringList overviewIds() const;
    void refreshOverview(bool reset=false);
    void prepareThumbnails();
    void scheduleThumbnailPreparation();
    QStringList thumbnailWorkingIds() const;
    QStringList prepareProjects();
protected:
    void closeEvent(QCloseEvent*) override;
    void changeEvent(QEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
private:
    void options(const QString& id);
    void switchInspectionMode(int index);
    void populatePreviewMenu(QMenu*, const QString& id, bool context);
    void cameraViews(QMenu*,const QString& id);
    void saveScreenshot(const QString& id, bool clipboard=false, bool wholeWindow=false);
    void selectCard(const QString& id);
    void markCard(const QString& id, Qt::KeyboardModifiers modifiers);
    void refreshMarked();
    void closeMarked();
    void hoverCard(const QString& id, const QPoint& position, bool explicitInput=false);
    void setupMenus(QToolButton* openButton);
    void toggleFullscreen(bool focusPreview=false);
    void leaveFullscreen();
    void fullscreenChanged();
    void resizeCodex();
    void populateSettings(QMenu*,const QString&,const QJsonObject&,const std::function<void(QJsonObject)>&,bool defaults=false);
    QAction *closeAction, *closeSelectedAction, *reopenAction, *saveAction, *copyAction;
    QAction* fitAction;
    QList<QAction*> modelActions;
    QList<QShortcut*> modelShortcuts;
    QCheckBox* syncButton;
    Service* service;
    QWidget* canvas;
    QGridLayout* grid;
    QLabel* position;
    QComboBox* selection=nullptr;
    QComboBox* inspectionMode=nullptr;
    QToolButton* previous;
    QToolButton* next;
    QComboBox* density;
    QToolButton* closeAllButton;
    OverviewGrid* overview=nullptr;
    ProjectGrid* projectGrid=nullptr;
    QWidget* projectTools=nullptr;
    QComboBox* projectFilter=nullptr;
    QComboBox* projectDensity=nullptr;
    QCheckBox* groupProjects=nullptr;
    QSet<QString> marked;
    QString markAnchor;
    QPoint hoverPosition;
    QString hoverTarget;
    QDockWidget* codexDock = nullptr;
    CodexPanel* codexPanel = nullptr;
    double codexFraction = 7.0/16.0;
    bool resizingCodex = false, codexResizePending = false;
    QDockWidget* inspectionDock = nullptr;
    InspectionPanel* inspectionPanel = nullptr;
    QAction* inspectAction = nullptr;
    QAction* fullscreenAction = nullptr;
    QShortcut* exitFullscreen = nullptr;
    FullscreenNotice* fullscreenNotice = nullptr;
    bool fullscreenFocus = false, fullscreenMode = false;
    bool thumbnailWatchScheduled=false,thumbnailPrepareScheduled=false;
    QTimer thumbnailInputQuiet;
    Qt::WindowStates windowedState;
    QRect windowedGeometry;
};

class Service final : public QObject {
    Q_OBJECT
public:
    Service(QString socketPath, QString statePath);
    ~Service() override;
    bool start(QString& error);
    QJsonObject request(const QJsonObject&);
    void reconcile();
    QJsonObject navigate(int delta);
    void save();
    void inspectionChanged(const QString& id,const QJsonObject& state,bool persist);
    void syncCamera(const QString& id);
    QJsonObject inspectionRequest(const QJsonObject&);
    QJsonObject annotationRequest(const QJsonObject&);
    QJsonObject inspectionState(const QString& id);
    void inspectionEvent(const QString& id);
    int pageSize() const { return window && window->isolatesPreview()?1:layout=="grid"?gridSize*gridSize:1; }
    QStringList order;
    QStringList comparison;
    bool cameraSync=true;
    bool syncingCamera=false;
    int projectDirection=1;
    QMap<QString, std::shared_ptr<Entry>> entries;
    QString selected, layout = "single", socketPath, statePath;
    QString thumbnailDirectory;
    QString inspectionId;
    QJsonObject codexOrigin;
    int gridSize = 2;
    QJsonObject projects;
    QStringList projectOrder;
    QString projectFilterId;
    bool groupByProject = true;
    int projectGridSize = 0;
    QJsonArray projectRows() const;
    QString projectCurrent(const QString& id) const;
    QStringList overviewOrder() const;
    preview::PreviewCache cache;
    std::unique_ptr<PreviewWindow> window;
private:
    friend class PreviewWindow;
    bool activationScheduled=false;
    bool eventFilter(QObject*, QEvent*) override;
    QJsonObject add(QJsonObject config, bool restore = false, bool makeCurrent = true, const QString& projectLabel = {});
    QJsonObject select(const QString& id,bool updateViews=true,bool single=false,bool makeCurrent=false);
    QString closeTarget(const QString& id) const;
    void preloadReturnTarget(bool settled=false);
    QString thumbnailKey(Entry&);
    void rememberThumbnail(Entry&);
    QString validate(QJsonObject& config, bool restore = false);
    QStringList dependencies(const Entry&) const;
    PublicationSnapshot outputStamps(const Entry&) const;
    bool currentPublication(const Entry&) const;
    void recordPublication(Entry&);
    void watchers();
    void changed();
    void reload(const QString& id);
    void activate(const QString& id);
    void deactivate(const QString& id);
    void build(const QString& id);
    void stopBuilder(Entry&);
    void status(Entry&, const QString&);
    QJsonObject describe(const Entry&) const;
    void replaceInspectionView(const QString& id);
    void cacheReview(Entry&);
    void updateInspectionMarkers(const QString& id);
    QString theme = "auto";
    QString closeBehavior = "left";
    PreviewPreloader preloader{&cache};
    QTimer projectPreloadDelay;
    QString pendingProjectPreload;
    QJsonObject preferences;
    QStringList selectionHistory;
    struct ClosedEntry { QJsonObject config; int index; };
    QList<ClosedEntry> closedEntries;
    QLocalServer server;
    QFileSystemWatcher watcher;
    QTimer debounce;
    int clients = 0;
    bool releasedViews = false;
};
