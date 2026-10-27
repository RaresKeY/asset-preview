#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QPointer>
class QLabel;
class QVBoxLayout;
class CodexPanel final : public QWidget {
    Q_OBJECT
public:
    explicit CodexPanel(QWidget* parent=nullptr);
    ~CodexPanel() override;
    bool attach(const QJsonObject& origin);
    void detach();
    bool pasteImage(const QString& path, const QString& context={});
    QJsonObject describe(QWidget* relativeTo) const;
    bool ownsFocus() const;
private:
    void terminalExited(QObject* terminal);
    QLabel* status;
    QVBoxLayout* layout;
    QPointer<QObject> part;
    QObject* partIdentity=nullptr;
    QJsonObject origin;
    QPointer<QWidget> terminalWidget;
    bool exited=false;
    struct Session { QPointer<QObject> part; QObject* identity; QPointer<QWidget> widget; QJsonObject origin; bool exited; };
    QList<Session> retained;
};
// Registration validates syntax; attachment also checks the live socket owner.
QString validateCodexOrigin(const QJsonValue& value);
