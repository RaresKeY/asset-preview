#pragma once

#include <QJsonObject>
#include <QString>

namespace preview {

// Lives outside QApplication/Service so their startup and teardown are covered.
// Normal diagnostics stay in RAM; fatal handlers use only fixed buffers/syscalls.
class CrashLog {
public:
    explicit CrashLog(const QString& statePath);
    ~CrashLog();
    CrashLog(const CrashLog&) = delete;
    CrashLog& operator=(const CrashLog&) = delete;
    static QString stateArgument(int argc, char** argv);
    static void context(const QJsonObject& state);
    bool beginSession(QString& error); // Only after acquiring the singleton lock.
    int finish(int code) { exitCode = code; return code; }
private:
    int exitCode = 0;
    bool session = false;
};

}
