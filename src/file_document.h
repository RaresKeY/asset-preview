#pragma once
#include <QString>
#include <QVector>

namespace FilePreview {
constexpr qint64 MaxBytes=2*1024*1024;
constexpr int MaxLines=20000;
struct Row {
    QChar kind=' ';
    QString text;
    int before=0, after=0;
};
struct Change {
    QString before, after;
    QVector<Row> rows;
    int added=0, removed=0;
};
struct Document {
    QString text;
    QVector<Change> changes;
    bool markdown=false, patch=false;
};
bool isTextPath(const QString&);
bool readText(const QString&,QString&,QString&);
bool load(const QString& path,const QString& before,Document&,QString& error);
QVector<Change> parsePatch(const QString&);
Change compare(const QString& before,const QString& after);
}
