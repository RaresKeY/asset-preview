#pragma once
#include <QJsonObject>
#include <QString>
#include <memory>

class QLockFile;
namespace preview {
class CacheStore;
// A finite, visible-load operation. The lease prevents eviction during import.
struct TexturePreview {
    QString path;
    QJsonObject metrics;
    std::unique_ptr<QLockFile> lease;
    TexturePreview();
    ~TexturePreview();
    TexturePreview(TexturePreview&&) noexcept;
    TexturePreview& operator=(TexturePreview&&) noexcept;
};
TexturePreview texturePreview(const QString& source,CacheStore* observer=nullptr);
}
