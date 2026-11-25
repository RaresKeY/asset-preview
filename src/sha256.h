#pragma once
#include <QCryptographicHash>

namespace preview {
inline QByteArray sha256(QByteArrayView bytes) {
    // Qt's one-shot API can repeatedly initialize/retire its crypto provider.
    // Reuse per-thread crypto state without memoizing mutable file identities.
    // Reset makes each call independent; result() returns an owning byte array.
    thread_local QCryptographicHash digest(QCryptographicHash::Sha256);
    digest.reset();
    digest.addData(bytes);
    return digest.result();
}
}
