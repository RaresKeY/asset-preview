#pragma once
#include <QScrollArea>
#include <QJsonArray>
#include <QMap>
#include <QPointer>
#include <functional>
class QComboBox;
class QLabel;
class QVBoxLayout;

// The panesById exist for every project. Only intersecting panesById own live previews.
class ProjectGrid final : public QScrollArea {
public:
    explicit ProjectGrid(QWidget* parent=nullptr);
    void setProjects(const QJsonArray&,int density);
    QStringList visibleIds() const;
    QString adjacentHovered(int delta) const;
    void attach(const QString& id,QWidget* card);
    QWidget* cardParent(const QString& id) const;
    int count() const { return order.size(); }
    QJsonObject describe(QWidget* window) const;
    std::function<void()> viewportChanged;
    std::function<void(QString,QString)> chosen;
protected:
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
private:
    struct Slot {
        QWidget* pane=nullptr;
        QLabel* label=nullptr;
        QLabel* mark=nullptr;
        QLabel* empty=nullptr;
        QComboBox* choices=nullptr;
        QVBoxLayout* body=nullptr;
        QPointer<QWidget> card;
        QString current;
    };
    void reflow();
    void schedule();
    void detach(Slot&);
    QWidget* content;
    QLabel* emptyState;
    QMap<QString,Slot> panesById;
    QStringList order;
    int density=0,columns=1,rows=1;
    bool pending=false;
};
