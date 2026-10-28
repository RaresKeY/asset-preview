#pragma once
#include "preview_cache.h"
#include <QObject>
#include <QElapsedTimer>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
class QProcess;
class QTemporaryDir;

// One window-owned scheduler, shared by Overview and ordinary project layouts.
// All identities/cache publications stay on the GUI thread; decodes are frozen
// requests and model jobs retain their independent hard-cancellation boundary.
class ThumbnailQueue final : public QObject {
public:
    struct Item { QString id,path,kind; bool operator==(const Item&) const = default; };
    ThumbnailQueue(preview::PreviewCache*,QString directory,QObject* parent=nullptr);
    ~ThumbnailQueue() override;
    void setItems(const QList<Item>&);
    void demand(QStringList visible,QStringList nearby,QSet<QString> excluded,bool enabled,bool warm);
    void refresh(bool reset=false);
    QSet<QString> loadingIds() const;
    QStringList workingIds() const;
    QJsonObject metrics() const;
    bool backgroundAllowed() const { return enabled && warm && !backgroundSuppressed; }
    std::function<QString(const QString&)> keyFor;
    std::function<QJsonObject(const QString&)> configFor;
    std::function<void(const QString&,const QString&)> changed;
    std::function<void()> activityChanged;
    std::function<void()> sourcesChanged;
private:
    struct Decode { QString id,key; bool foreground; std::shared_ptr<std::atomic_bool> cancelled; };
    void schedule(int delay=25,bool debounce=true);
    void work();
    void decode(const Item&,const QString&,bool);
    void render(const Item&,const QString&,bool);
    void cancelRender();
    void publish(const QString&,const QImage&);
    void notify();
    bool eligible(const QString&) const;
    bool inFlight(const QString&) const;
    preview::PreviewCache* cache;
    QString root;
    QList<Item> items;
    QMap<QString,Item> byId;
    QStringList visible,nearby;
    QSet<QString> excluded,dirty,hydrated,attempted;
    QMap<QString,QString> keys;
    QMap<QString,Decode> decodes;
    QThreadPool pool;
    QTimer dispatch,deadline;
    QElapsedTimer quiet;
    bool debouncePending=false;
    bool enabled=false,warm=false,backgroundSuppressed=false,renderForeground=false,cancelled=false,timedOut=false;
    QPointer<QProcess> process;
    std::unique_ptr<QTemporaryDir> directory;
    QString renderId,renderKey,lastRenderer,lastError;
    int rendered=0,decoded=0,peakImages=0;
};
