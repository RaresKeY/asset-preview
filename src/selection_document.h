#pragma once
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace Selection {
struct Item {
    QString id, title, markdown, recommendation, digest;
    QString section, sectionTitle, sectionMarkdown;
    QString image, asset;
};
struct Document {
    QString id, title, markdown, path, digest;
    QVector<Item> items;
    bool variantRows=false;
};
QString defaultDecisionsPath(const QString& path);
bool isImagePath(const QString& path);
bool load(const QString& path, Document&, QString& error);
bool readDecisions(const QString& path, const Document&, QJsonObject&, QString& error);
QString decision(const QJsonObject&, const Item&);
bool writeDecision(const QString& path, const Document&, const QString& itemId,
                   const QString& value, QJsonObject& result, QString& error);
}
