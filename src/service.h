#pragma once
#include "viewer.h"
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QLocalServer>
#include <QMainWindow>
#include <QMap>
#include <QProcess>
#include <QTimer>
#include <memory>

class QMenu;
class QAction;
class QShortcut;
class QLabel;
class QComboBox;
class QGridLayout;
class QFrame;
class QCheckBox;
class QToolButton;
class Service;

struct Entry {
    QJsonObject config;
    QString status = "Suspended", log;
    QMap<QString, QString> stamps;
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
};

class PreviewWindow final : public QMainWindow {
public:
    explicit PreviewWindow(Service* service);
    void navigation();
    void arrange(const QStringList& ids);
    void createCard(Entry& entry);
    void removeCard(Entry& entry);
    void reportError(const QString& error);
protected:
    void closeEvent(QCloseEvent*) override;
    void changeEvent(QEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
private:
    void options(const QString& id);
    void populatePreviewMenu(QMenu*, const QString& id, bool context);
    void cameraViews(QMenu*,const QString& id);
    void saveScreenshot(const QString& id, bool clipboard=false, bool wholeWindow=false);
    void selectCard(const QString& id);
    void setupMenus(QToolButton* openButton);
    void populateSettings(QMenu*,const QString&,const QJsonObject&,const std::function<void(QJsonObject)>&);
    QAction *closeAction, *reopenAction, *saveAction, *copyAction;
    QAction* fitAction;
    QList<QAction*> modelActions;
    QList<QShortcut*> modelShortcuts;
    QCheckBox* syncButton;
    Service* service;
    QWidget* canvas;
    QGridLayout* grid;
    QLabel* position;
    QComboBox* selection=nullptr;
    QToolButton* previous;
    QToolButton* next;
    QComboBox* density;
    QCheckBox* compactButton;
    QToolButton* closeAllButton;
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
    int pageSize() const { return layout=="grid" ? gridSize*gridSize : 1; }
    QStringList order;
    QStringList comparison;
    bool cameraSync=true;
    bool syncingCamera=false;
    QMap<QString, std::shared_ptr<Entry>> entries;
    QString selected, layout = "single", socketPath, statePath;
    int gridSize = 2;
    bool compact = false;
    std::unique_ptr<PreviewWindow> window;
private:
    friend class PreviewWindow;
    QJsonObject add(QJsonObject config, bool restore = false);
    QString validate(QJsonObject& config, bool restore = false);
    QStringList dependencies(const Entry&) const;
    void watchers();
    void changed();
    void reload(const QString& id);
    void activate(const QString& id);
    void deactivate(const QString& id);
    void build(const QString& id);
    void stopBuilder(Entry&);
    void status(Entry&, const QString&);
    QJsonObject describe(const Entry&) const;
    QJsonObject preferences;
    QStringList selectionHistory;
    struct ClosedEntry { QJsonObject config; int index; };
    QList<ClosedEntry> closedEntries;
    QLocalServer server;
    QFileSystemWatcher watcher;
    QTimer debounce;
    int clients = 0;
};
