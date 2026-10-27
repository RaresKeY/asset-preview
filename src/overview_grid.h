#pragma once
#include "preview_cache.h"
#include <QAbstractScrollArea>
#include <QSet>
#include <QMap>
#include <QThreadPool>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>
class QProcess;
class QTemporaryDir;
class QTimer;

class OverviewGrid final : public QAbstractScrollArea {
public:
    struct Item { QString id,label,path,kind,project,projectLabel; };
    OverviewGrid(preview::PreviewCache*,const QString& jobDirectory,QWidget* parent=nullptr);
    ~OverviewGrid() override;
    void setItems(QList<Item>,const QString& selected,bool grouped=false);
    void select(const QString&,bool reveal=false);
    void setMarked(const QSet<QString>& ids);
    QStringList visibleIds() const;
    QJsonObject describe(QWidget* window) const;
    void invalidate();
    void refresh();
    std::function<QString(const QString&)> keyFor;
    std::function<QJsonObject(const QString&)> configFor;
    std::function<void(const QString&)> activated,selected;
    std::function<void(const QString&,Qt::KeyboardModifiers)> marked;
    std::function<void()> viewportChanged;
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool viewportEvent(QEvent*) override;
private:
    int columns() const;
    QRect tile(int index) const;
    QRect checkbox(int index) const;
    void reflow();
    void updateTile(const QString&);
    void schedule(int delay=25);
    void work();
    void render(const Item&,const QString& key);
    void cancelRender();
    preview::PreviewCache* cache;
    QList<Item> items;
    QString current;
    bool grouped=false;
    QList<QRect> tiles;
    struct Heading { QString id,label; QRect rect; };
    QList<Heading> headings;
    QSet<QString> checked;
    int hover=-1,epoch=0;
    bool busy=false;
    QMap<QString,QString> attempted;
    QMap<QString,QString> keys;
    QTimer spinner,dispatch;
    QString loadingId;
    int spinAngle=0;
    QThreadPool worker;
    QPointer<QProcess> renderWorker;
    QString renderRoot;
    std::unique_ptr<QTemporaryDir> renderDirectory;
    QTimer* renderDeadline;
    QString renderingId,renderingKey,lastRenderer,lastRenderError;
    int rendered=0;
    bool renderCancelled=false,renderTimedOut=false;
};
