#pragma once
#include <QFrame>
#include <QJsonObject>
#include <QPropertyAnimation>
#include <QTimer>
#include <functional>

class QPainter;

class FullscreenNotice final : public QFrame {
public:
    explicit FullscreenNotice(QWidget* parent);
    void announce();
    void dismiss();
    void compose(QPainter&, QWidget* window);
    QJsonObject describe(QWidget* window) const;
    std::function<void()> exitRequested;
protected:
    bool eventFilter(QObject*, QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    void place();
    QTimer delay;
    QPropertyAnimation slide;
};
