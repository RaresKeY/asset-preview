#include "crash_log.h"
#include "backend_abi_config.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

namespace {
constexpr size_t LogLimit = 1024 * 1024, ContextLimit = 16 * 1024;
constexpr int MessageCount = 32, MessageLimit = 1024;
constexpr int FatalSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGSYS, SIGTRAP};
constexpr char Marker[] = "session.running.json";
int directory = -1, output = -1;
QtMessageHandler previousMessageHandler = nullptr;
std::terminate_handler previousTerminateHandler = nullptr;
struct sigaction previousSignals[std::size(FatalSignals)]{};
stack_t previousStack{};
alignas(16) char alternateStack[128 * 1024];
bool stackInstalled = false, handlersInstalled = false;
volatile sig_atomic_t ownsSession = 0;
volatile sig_atomic_t writeFailed = 0, reportSaved = 0;
std::atomic_flag buffersBusy = ATOMIC_FLAG_INIT, reporting = ATOMIC_FLAG_INIT;
char messages[MessageCount][MessageLimit], contextBytes[ContextLimit];
size_t messageSizes[MessageCount]{}, contextSize = 0;
unsigned messageIndex = 0, messageTotal = 0;
QJsonObject currentContext;
QByteArray sessionInfo;
char buildInfo[8192]; size_t buildInfoSize = 0;

bool writeBytes(int fd, const char* bytes, size_t count) {
    while (count) {
        const auto n = ::write(fd, bytes, count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (fd == output) writeFailed = 1; return false; }
        bytes += n; count -= size_t(n);
    }
    return true;
}
template<size_t N> void literal(const char (&text)[N]) { writeBytes(output, text, N-1); }

// No printf, Qt, allocation, unwinder or locks in the fatal-signal path.
struct Text {
    char bytes[2048]; size_t size = 0;
    void text(const char* s) { while (*s && size < sizeof(bytes)) bytes[size++] = *s++; }
    void number(uint64_t n, unsigned base = 10, unsigned width = 0) {
        char digits[32]; unsigned count = 0;
        do { digits[count++] = "0123456789abcdef"[n % base]; n /= base; } while (n);
        while (count < width) digits[count++] = '0';
        while (count && size < sizeof(bytes)) bytes[size++] = digits[--count];
    }
    void signedNumber(int n) { if (n < 0) text("-"); number(n < 0 ? uint64_t(-int64_t(n)) : uint64_t(n)); }
    void hex(uintptr_t n) { text("0x"); number(n, 16); }
    void flush() { writeBytes(output, bytes, size); size = 0; }
};
bool leap(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
void timestamp(Text& text) {
    timespec now{}; ::clock_gettime(CLOCK_REALTIME, &now);
    uint64_t days = uint64_t(now.tv_sec) / 86400, seconds = uint64_t(now.tv_sec) % 86400;
    int year = 1970, month = 1;
    while (days >= unsigned(leap(year) ? 366 : 365)) days -= leap(year++) ? 366 : 365;
    constexpr unsigned months[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    while (days >= months[month-1] + unsigned(month == 2 && leap(year))) {
        days -= months[month-1] + unsigned(month == 2 && leap(year)); ++month;
    }
    text.number(year); text.text("-"); text.number(month,10,2); text.text("-"); text.number(days+1,10,2);
    text.text("T"); text.number(seconds/3600,10,2); text.text(":"); text.number(seconds/60%60,10,2);
    text.text(":"); text.number(seconds%60,10,2); text.text("."); text.number(now.tv_nsec/1000000,10,3); text.text("Z");
}
const char* signalName(int signal) {
    switch (signal) {
    case SIGSEGV: return "SIGSEGV"; case SIGABRT: return "SIGABRT"; case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL"; case SIGFPE: return "SIGFPE"; case SIGSYS: return "SIGSYS"; case SIGTRAP: return "SIGTRAP";
    default: return "unknown";
    }
}
int privateFile(const char* name, int flags) {
    int fd = ::openat(directory, name, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat info{};
    if (fd < 0) return -1;
    if (fd <= STDERR_FILENO) {
        const int replacement = ::fcntl(fd,F_DUPFD_CLOEXEC,3);
        ::close(fd); fd = replacement;
        if (fd < 0) return -1;
    }
    if (::fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != ::getuid() || info.st_nlink != 1) {
        ::close(fd); return -1;
    }
    if (::fchmod(fd, 0600)) { ::close(fd); return -1; }
    return fd;
}
void rotate() {
    struct stat info{};
    // Reserve enough room for a complete bounded report before appending it.
    if (!::fstat(output, &info) && info.st_size > off_t(LogLimit - 192 * 1024)) {
        if (::renameat(directory, "crash.log", directory, "crash.log.1")) return;
        const int replacement = privateFile("crash.log", O_WRONLY | O_CREAT | O_APPEND);
        if (replacement >= 0) { ::close(output); output = replacement; }
    }
}
void moduleMap() {
    literal("\nloaded_modules (/proc/self/maps, at most 96 KiB):\n");
    const int maps = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (maps < 0) return;
    char chunk[4096]; size_t total = 0;
    while (total < 96 * 1024) {
        const auto n = ::read(maps, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        writeBytes(output, chunk, size_t(n)); total += size_t(n);
    }
    ::close(maps);
}
void faultContext(void* opaque) {
    if (!opaque) return;
    auto* context = static_cast<ucontext_t*>(opaque);
    uintptr_t pc = 0, sp = 0, frame = 0;
#if defined(__x86_64__)
    pc = context->uc_mcontext.gregs[REG_RIP]; sp = context->uc_mcontext.gregs[REG_RSP]; frame = context->uc_mcontext.gregs[REG_RBP];
#elif defined(__aarch64__)
    pc = context->uc_mcontext.pc; sp = context->uc_mcontext.sp; frame = context->uc_mcontext.regs[29];
#endif
    Text text; text.text("instruction_pointer="); text.hex(pc); text.text(" stack_pointer="); text.hex(sp);
    text.text("\nnative_frames (instruction pointer, then best-effort frame pointers):\n"); text.hex(pc); text.text("\n");
#if defined(SYS_process_vm_readv)
    // Read frames through the kernel so a corrupt stack pointer cannot cause a
    // second fault. Optimized third-party libraries may end this chain early.
    for (unsigned i = 0; i < 32 && frame >= sp && frame-sp < 16*1024*1024 && frame % sizeof(uintptr_t) == 0; ++i) {
        uintptr_t pair[2]{};
        iovec local{pair,sizeof(pair)}, remote{reinterpret_cast<void*>(frame),sizeof(pair)};
        if (::syscall(SYS_process_vm_readv, ::getpid(), &local, 1, &remote, 1, 0) != ssize_t(sizeof(pair))) break;
        text.hex(pair[1]); text.text("\n");
        if (pair[0] <= frame) break;
        frame = pair[0];
    }
#endif
    text.flush();
}
bool report(const char* event, int signal = 0, siginfo_t* info = nullptr, void* fault = nullptr, int exitCode = 0,
            const char* prior = nullptr, size_t priorSize = 0) {
    if (output < 0 || reporting.test_and_set()) return false;
    writeFailed = 0; reportSaved = 0;
    rotate();
    Text text; text.text("\n=== Asset Preview crash ===\ntime_utc="); timestamp(text);
    text.text("\npid="); text.number(::getpid()); text.text(" tid="); text.number(::syscall(SYS_gettid));
    text.text("\nevent="); text.text(event); text.text("\n");
    if (signal) {
        text.text("signal="); text.text(signalName(signal)); text.text(" ("); text.number(signal); text.text(")");
        if (info) { text.text(" code="); text.signedNumber(info->si_code); if (info->si_code > 0) { text.text(" address="); text.hex(uintptr_t(info->si_addr)); } }
        text.text("\n");
    }
    if (exitCode) { text.text("exit_code="); text.signedNumber(exitCode); text.text("\n"); }
    text.flush();
    writeBytes(output, buildInfo, buildInfoSize);
    if (prior) { literal("previous_session="); writeBytes(output, prior, priorSize); literal("\nSignal/cause unavailable: the previous process could not report its exit.\n"); }
    // A signal can interrupt the diagnostic writer. Never wait for its lock.
    const bool acquired = !buffersBusy.test_and_set();
    if (acquired) {
        literal("preview_context="); writeBytes(output, contextBytes, contextSize);
        literal("\nrecent_qt_diagnostics (RAM only until failure):\n");
        const auto count = std::min(messageTotal, unsigned(MessageCount));
        for (unsigned i = 0; i < count; ++i) {
            const unsigned slot = (messageIndex + MessageCount - count + i) % MessageCount;
            writeBytes(output, messages[slot], messageSizes[slot]);
        }
    } else literal("preview_context/recent_diagnostics unavailable: interrupted during capture\n");
    if (fault) { faultContext(fault); moduleMap(); }
    literal("=== End crash ===\n");
    reportSaved = ::fsync(output) == 0 && !writeFailed;
    if (reportSaved && ownsSession) { ::unlinkat(directory, Marker, 0); ::fsync(directory); }
    if (acquired) buffersBusy.clear();
    reporting.clear();
    return reportSaved;
}
void fatalSignal(int signal, siginfo_t* info, void* context) {
    report("fatal_signal", signal, info, context);
    // Preserve the real signal and the host's normal core-dump handling.
    struct sigaction action{}; action.sa_handler = SIG_DFL; ::sigemptyset(&action.sa_mask);
    ::sigaction(signal, &action, nullptr);
    sigset_t unblocked; ::sigemptyset(&unblocked); ::sigaddset(&unblocked, signal);
    ::sigprocmask(SIG_UNBLOCK, &unblocked, nullptr);
    ::syscall(SYS_tgkill, ::getpid(), ::syscall(SYS_gettid), signal);
    ::_exit(128 + signal);
}
void qtMessage(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const char* severity = type == QtDebugMsg ? "debug" : type == QtInfoMsg ? "info" : type == QtWarningMsg ? "warning" : type == QtCriticalMsg ? "critical" : "fatal";
    const auto bytes = (QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + " " + severity + " " + qFormatLogMessage(type,context,message)).toUtf8().left(MessageLimit-2) + '\n';
    if (!buffersBusy.test_and_set()) {
        std::memcpy(messages[messageIndex], bytes.constData(), size_t(bytes.size()));
        messageSizes[messageIndex] = size_t(bytes.size()); messageIndex = (messageIndex+1) % MessageCount;
        if (messageTotal < MessageCount) ++messageTotal;
        buffersBusy.clear();
    }
    // Direct/foreground runners can still collect stderr explicitly. Source
    // desktop launches discard it instead of writing an always-on session log.
    writeBytes(STDERR_FILENO, bytes.constData(), size_t(bytes.size()));
}
void terminateException() {
    // Capture the exception before abort; libstdc++'s default stderr message is
    // otherwise discarded by ordinary desktop launchers.
    try {
        if (const auto exception = std::current_exception()) std::rethrow_exception(exception);
        qCritical("C++ std::terminate without an active exception");
    } catch (const std::exception& exception) {
        qCritical("Uncaught C++ exception: %s",exception.what());
    } catch (...) { qCritical("Uncaught C++ exception of unknown type"); }
    std::abort();
}
QJsonObject registryContext(const QString& statePath) {
    QFile file(statePath + "/previews.json");
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4*1024*1024) return {};
    const auto registry = QJsonDocument::fromJson(file.readAll()).object();
    auto ids = registry["comparison"].toArray(); ids.append(registry["selected"]);
    QJsonArray previews;
    for (const auto& entry : registry["entries"].toArray()) {
        const auto row = entry.toObject();
        if (ids.contains(row["id"])) previews.append(QJsonObject{{"id",row["id"]},{"path",row["path"]},{"kind",row["kind"]},{"settings",row["settings"]}});
    }
    return {{"selected",registry["selected"]},{"comparison",registry["comparison"]},{"layout",registry["layout"]},{"previews",previews}};
}
}

namespace preview {
QString CrashLog::stateArgument(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == "--state" && i+1 < argc) return QString::fromLocal8Bit(argv[i+1]);
        if (argument.startsWith("--state=")) return argument.mid(8);
    }
    return {};
}
CrashLog::CrashLog(const QString& statePath) {
    if (statePath.isEmpty()) return;
    const mode_t mask = ::umask(0077);
    const bool created = QDir().mkpath(statePath);
    ::umask(mask);
    if (!created) return;
    directory = ::open(QFile::encodeName(statePath).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info{};
    if (directory < 0 || ::fstat(directory,&info) || info.st_uid != ::getuid() || (info.st_mode & 0022)) return;
    output = privateFile("crash.log", O_WRONLY | O_CREAT | O_APPEND);
    if (output < 0) return;
    char executable[4096]{};
    const auto size = ::readlink("/proc/self/exe",executable,sizeof(executable)-1);
    struct stat binary{}; ::stat("/proc/self/exe",&binary);
    sessionInfo = "executable=" + QByteArray(executable,size > 0 ? int(size) : 0)
        + "\nversion=" ASSET_PREVIEW_VERSION "\nbackend_abi=" ASSET_PREVIEW_BACKEND_ABI
        + "\nbinary_size=" + QByteArray::number(binary.st_size) + " binary_mtime=" + QByteArray::number(binary.st_mtime) + '\n';
    buildInfoSize = std::min(size_t(sessionInfo.size()),sizeof(buildInfo));
    std::memcpy(buildInfo,sessionInfo.constData(),buildInfoSize);
    CrashLog::context(registryContext(statePath));
    stack_t stack{}; stack.ss_sp = alternateStack; stack.ss_size = sizeof(alternateStack);
    stackInstalled = ::sigaltstack(&stack,&previousStack) == 0;
    for (size_t i = 0; i < std::size(FatalSignals); ++i) {
        struct sigaction action{}; action.sa_sigaction = fatalSignal; ::sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        ::sigaction(FatalSignals[i], &action, &previousSignals[i]);
    }
    handlersInstalled = true;
    previousMessageHandler = qInstallMessageHandler(qtMessage);
    previousTerminateHandler = std::set_terminate(terminateException);
}
CrashLog::~CrashLog() {
    if (exitCode && output >= 0) report("nonzero_exit",0,nullptr,nullptr,exitCode);
    if (session && directory >= 0 && (!exitCode || reportSaved)) { ::unlinkat(directory,Marker,0); ::fsync(directory); }
    if (handlersInstalled) {
        qInstallMessageHandler(previousMessageHandler);
        std::set_terminate(previousTerminateHandler);
        for (size_t i = 0; i < std::size(FatalSignals); ++i) ::sigaction(FatalSignals[i],&previousSignals[i],nullptr);
    }
    if (stackInstalled) ::sigaltstack(&previousStack,nullptr);
    if (output >= 0) ::close(output);
    if (directory >= 0) ::close(directory);
}
void CrashLog::context(const QJsonObject& state) {
    if (output < 0) return;
    for (auto it = state.begin(); it != state.end(); ++it) currentContext[it.key()] = it.value();
    const auto bytes = QJsonDocument(currentContext).toJson(QJsonDocument::Compact).left(ContextLimit);
    if (!buffersBusy.test_and_set()) {
        std::memcpy(contextBytes,bytes.constData(),size_t(bytes.size())); contextSize = size_t(bytes.size());
        buffersBusy.clear();
    }
}
bool CrashLog::beginSession(QString& error) {
    if (output < 0) { error = "Cannot open a private crash.log in the state directory"; return false; }
    const int previous = privateFile(Marker,O_RDONLY);
    if (previous >= 0) {
        char prior[4096]; const auto size = ::read(previous,prior,sizeof(prior)); ::close(previous);
        if (size > 0 && !report("unclean_exit",0,nullptr,nullptr,0,prior,size_t(size))) {
            error = "Cannot persist the previous unclean-exit report"; return false;
        }
        ::unlinkat(directory,Marker,0);
    }
    const int marker = privateFile(Marker,O_WRONLY | O_CREAT | O_EXCL);
    if (marker < 0) { error = "Cannot create the crash recovery session marker"; return false; }
    const auto data = QJsonDocument(QJsonObject{{"pid",qint64(::getpid())},{"started_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"build",QString::fromUtf8(sessionInfo)}}).toJson(QJsonDocument::Compact);
    const bool written = writeBytes(marker,data.constData(),size_t(data.size())) && ::fsync(marker) == 0;
    ::close(marker);
    if (!written) { ::unlinkat(directory,Marker,0); error = "Cannot write the crash recovery session marker"; return false; }
    ::fsync(directory);
    session = true; ownsSession = 1;
    return true;
}
}
