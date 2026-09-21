#include "CrashHandler.h"

#ifdef __linux__

#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace {

int g_crashLogFd = -1;

void WriteAll(const char* data, size_t len) {
    // 크래시 중에는 write 실패를 처리할 방법이 없으므로 반환값은 무시한다.
    ssize_t r = write(STDERR_FILENO, data, len);
    (void)r;
    if (g_crashLogFd >= 0) {
        r = write(g_crashLogFd, data, len);
        (void)r;
    }
}

const char* SignalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        default:      return "?";
    }
}

void CrashHandler(int sig) {
    // 핸들러 안에서 또 크래시하면 무한 재귀하지 않도록 기본 동작으로 넘긴다.
    static volatile sig_atomic_t entered = 0;
    if (entered) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    entered = 1;

    char threadName[32] = "?";
    pthread_getname_np(pthread_self(), threadName, sizeof(threadName));

    char header[160];
    int len = snprintf(header, sizeof(header), "\n=== FATAL %s (signal %d) in thread '%s' ===\n",
                       SignalName(sig), sig, threadName);
    if (len > 0) {
        size_t n = static_cast<size_t>(len);
        if (n >= sizeof(header)) n = sizeof(header) - 1;
        WriteAll(header, n);
    }

    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    if (g_crashLogFd >= 0) {
        backtrace_symbols_fd(frames, count, g_crashLogFd);
    }

    const char footer[] = "=== end of backtrace ===\n";
    WriteAll(footer, sizeof(footer) - 1);

    // 기본 동작으로 되돌려 다시 발생시킨다 -> 코어 덤프와 종료 코드(139 등)가 그대로 유지된다.
    signal(sig, SIG_DFL);
    raise(sig);
}

}  // namespace

void InstallCrashHandler() {
    // backtrace()는 처음 호출할 때 libgcc를 로드하면서 malloc을 쓸 수 있다.
    // 시그널 핸들러 안에서 처음 호출되지 않도록 미리 한 번 불러둔다.
    void* warmup[1];
    backtrace(warmup, 1);

    g_crashLogFd = open("crash-backtrace.log", O_WRONLY | O_CREAT | O_APPEND, 0644);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = CrashHandler;
    sigemptyset(&sa.sa_mask);

    const int signals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
    for (int s : signals) {
        sigaction(s, &sa, nullptr);
    }
}

#else

void InstallCrashHandler() {}

#endif
