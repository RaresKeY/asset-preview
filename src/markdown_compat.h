#pragma once
#include <QString>
class QTextDocument;

namespace FilePreview {
// Recognize presentation-only HTML without enabling Qt's lossy HTML importer.
void setMarkdown(QTextDocument* document,const QString& source);
}
