#include "backend_abi_config.h"
#include <QtCore/qglobal.h>

// Keep this handshake C-only: shared C++ types are not safe until it succeeds.
extern "C" Q_DECL_EXPORT const char* asset_preview_backend_abi() {
    return ASSET_PREVIEW_BACKEND_ABI;
}
