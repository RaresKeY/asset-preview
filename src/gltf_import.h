#pragma once
#include <QJsonObject>

namespace preview {
// Optional reader in the already lazy F3D module. No engine or cache retention.
void loadGltfImportPlugin();
void resetGltfImportMetrics();
QJsonObject gltfImportMetrics();
}
