#pragma once
#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace preview::inspection {
// Resolve an existing parent as well as existing files. A future output under a
// symlinked directory must have the same identity as its direct watched path.
inline QString pathIdentity(const QString& path) {
    const auto canonical=QFileInfo(path).canonicalFilePath();
    if (!canonical.isEmpty()) return canonical;
    QString parent=QFileInfo(path).absolutePath();
    QStringList suffix{QFileInfo(path).fileName()};
    while (QFileInfo(parent).canonicalFilePath().isEmpty() && parent!="/") {
        suffix.prepend(QFileInfo(parent).fileName()); parent=QFileInfo(parent).absolutePath();
    }
    return QDir(QFileInfo(parent).canonicalFilePath()).filePath(suffix.join('/'));
}
}
