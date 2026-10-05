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

class QLabel;
class QComboBox;
class QPushButton;
class QGridLayout;
class QFrame;
class Service;

struct Entry {
    QJsonObject config;
    QString status = "Suspended", log;
    QMap<QString, QString> stamps;
    PreviewView* view = nullptr;
    QWidget* surface = nullptr;
    QFrame* card = nullptr;
    QLabel* statusLabel = nullptr;
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
protected:
    void closeEvent(QCloseEvent*) override;
    void changeEvent(QEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
private:
    void options(const QString& id);
    Service* service;
    QWidget* canvas;
    QGridLayout* grid;
    QLabel* position;
    QComboBox* selection;
    QPushButton* previous;
    QPushButton* next;
    QPushButton* mode;
};

class Service final : public QObject {
    Q_OBJECT
public:
    Service(QString socketPath, QString statePath);
    ~Service() override;
    bool start(QString& error);
    QJsonObject request(const QJsonObject&);
    void reconcile();
    void navigate(int delta);
    void save();
    QStringList order;
    QMap<QString, std::shared_ptr<Entry>> entries;
    QString selected, layout = "single", socketPath, statePath;
    std::unique_ptr<PreviewWindow> window;
private:
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
    QLocalServer server;
    QFileSystemWatcher watcher;
    QTimer debounce;
    int clients = 0;
};
