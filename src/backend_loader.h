#pragma once
#include "backend_abi_config.h"
#include <QLibrary>
#include <cstring>
#include <stdexcept>

namespace preview {
inline QFunctionPointer resolveBackend(QLibrary& library, const char* symbol) {
    if (!library.load()) throw std::runtime_error(library.errorString().toStdString());
    using Signature = const char* (*)();
    const auto signature=reinterpret_cast<Signature>(library.resolve("asset_preview_backend_abi"));
    const char* actual=signature?signature():nullptr;
    if (!actual || std::strcmp(actual,ASSET_PREVIEW_BACKEND_ABI)!=0)
        throw std::runtime_error("Preview backend is incompatible with this running server. Finish building, then restart Asset Preview.");
    const auto factory=library.resolve(symbol);
    if (!factory) throw std::runtime_error(QString("Preview backend is missing factory %1: %2")
        .arg(QString::fromLatin1(symbol),library.errorString()).toStdString());
    return factory;
}
}
