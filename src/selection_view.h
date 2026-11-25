#pragma once
#include "viewer.h"
#include "selection_document.h"
#include <QFileSystemWatcher>
#include <QVector>
#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QScrollArea;
class QToolButton;
class QVBoxLayout;
class QHBoxLayout;
class QTimer;
class SelectionImage;

class SelectionView final : public QWidget, public PreviewView {
public:
    explicit SelectionView(QWidget* parent=nullptr);
    bool reload(const QJsonObject&, QString&) override;
    void fit() override;
    void settings(const QJsonObject&) override {}
    QJsonObject metrics() const override;
    QImage snapshot() override { return grab().toImage(); }
    void setCardWidgets(QWidget* actions, QWidget* status);
    std::function<void(QString, QString)> openPreview;
protected:
    void resizeEvent(QResizeEvent*) override;
    void changeEvent(QEvent*) override;
    void showEvent(QShowEvent*) override;
private:
    struct Row {
        QWidget* widget=nullptr;
        QToolButton *accept=nullptr,*deny=nullptr,*reset=nullptr;
        QLabel* state=nullptr;
        SelectionImage* image=nullptr;
        QScrollArea* strip=nullptr;
    };
    void rebuild();
    void refresh();
    void readResults();
    void choose(int, const QString&);
    void navigate(int);
    void theme();
    void refreshImages();
    Selection::Document document;
    QJsonObject results;
    QString resultPath, resultError;
    QVector<Row> rows;
    QVector<QWidget*> sections;
    QComboBox *mode,*filter;
    QLineEdit* search;
    QLabel *title,*counts,*notice,*position,*empty;
    QToolButton *previous,*next;
    QScrollArea* scroll;
    QVBoxLayout* contents;
    QHBoxLayout *toolbar,*searchbar;
    QWidget* searchTools;
    QTimer* imageRefresh;
    QFileSystemWatcher watcher;
    bool writable=false;
    bool sourceValid=false;
    bool changingTheme=false;
    int current=0,loads=0;
};
