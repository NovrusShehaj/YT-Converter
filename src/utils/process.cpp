#include "process.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef _WIN32
extern char** environ;
#endif

namespace yt::process {
namespace {

std::mutex g_mutex;
std::set<long long> g_children;
std::atomic<bool> g_shutdown{false};

void appendBounded(std::string& buffer, const char* data, std::size_t n, std::size_t maxBytes) {
    if (n == 0 || maxBytes == 0) {
        return;
    }
    buffer.append(data, n);
    if (buffer.size() > maxBytes) {
        buffer.erase(0, buffer.size() - maxBytes);
    }
}

// Pending callback line for one stream. Its storage never exceeds RunOptions::max_line_bytes.
struct LineBuffer {
    std::string pending;
    bool discarding = false;
};

void deliverLine(LineBuffer& line, const RunOptions& options) {
    // Reset before invoking the callback so a throwing callback leaves consistent state.
    std::string text;
    text.swap(line.pending);
    line.discarding = false;
    options.on_line(text, options.on_line_user);
}

void consumeChunk(std::string& buffer, LineBuffer& line, const char* data, std::size_t n,
                  const RunOptions& options, RunResult& result) {
    appendBounded(buffer, data, n, options.max_output_bytes);
    if (options.on_line == nullptr) {
        return;
    }
    const std::size_t limit = std::max<std::size_t>(1, options.max_line_bytes);
    for (std::size_t i = 0; i < n; ++i) {
        const char c = data[i];
        if (c == '\n') {
            deliverLine(line, options);
        } else if (c == '\r') {
            continue;
        } else if (line.pending.size() < limit) {
            line.pending.push_back(c);
            result.peak_line_bytes = std::max(result.peak_line_bytes, line.pending.size());
        } else {
            line.discarding = true;
        }
    }
}

void flushPending(LineBuffer& line, const RunOptions& options) {
    if (options.on_line != nullptr && (!line.pending.empty() || line.discarding)) {
        deliverLine(line, options);
    }
}

#ifndef _WIN32

struct Fd {
    int fd = -1;
    Fd() = default;
    explicit Fd(int value) : fd(value) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd(other.fd) { other.fd = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            close();
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
    ~Fd() { close(); }
    void close() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
    int release() {
        const int value = fd;
        fd = -1;
        return value;
    }
};

void registerChild(pid_t pid) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_children.insert(static_cast<long long>(pid));
}

void unregisterChild(pid_t pid) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_children.erase(static_cast<long long>(pid));
}

void killProcessGroup(pid_t pid, int sig) {
    if (pid <= 0) {
        return;
    }
    if (::kill(-pid, sig) != 0) {
        ::kill(pid, sig);
    }
}

int openDevNull() {
    return ::open("/dev/null", O_RDONLY | O_CLOEXEC);
}

int makePipe(int fds[2]) {
#ifdef __linux__
    return ::pipe2(fds, O_CLOEXEC);
#else
    if (::pipe(fds) != 0) {
        return -1;
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

void setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

// Reads whatever is available without blocking. A grandchild that inherited the pipe can keep it
// open after the direct child exits, so a blocking drain could hang forever.
void drainFd(int fd, std::string& buffer, LineBuffer& line, const RunOptions& options,
             RunResult& result) {
    char chunk[4096];
    while (true) {
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            consumeChunk(buffer, line, chunk, static_cast<std::size_t>(n), options, result);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

// Owns a spawned child until it is reaped. If run() unwinds early (for example a throwing line
// callback), the destructor kills the process group, reaps the child, and unregisters it.
class ChildGuard {
  public:
    explicit ChildGuard(pid_t pid) : pid_(pid) { registerChild(pid_); }
    ChildGuard(const ChildGuard&) = delete;
    ChildGuard& operator=(const ChildGuard&) = delete;
    ~ChildGuard() {
        if (!reaped_) {
            killProcessGroup(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
        unregisterChild(pid_);
    }
    void markReaped() { reaped_ = true; }

  private:
    pid_t pid_;
    bool reaped_ = false;
};

RunResult runPosix(const std::vector<std::string>& argv, const RunOptions& options) {
    RunResult result;
    std::vector<std::string> owned = argv;
    std::vector<char*> ptrs;
    ptrs.reserve(owned.size() + 1);
    for (auto& arg : owned) {
        ptrs.push_back(arg.data());
    }
    ptrs.push_back(nullptr);

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    Fd outRead;
    Fd errRead;
    Fd devNull(openDevNull());

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    bool actionsInit = false;
    bool attrsInit = false;

    auto cleanupSpawn = [&]() {
        if (actionsInit) {
            posix_spawn_file_actions_destroy(&actions);
            actionsInit = false;
        }
        if (attrsInit) {
            posix_spawnattr_destroy(&attrs);
            attrsInit = false;
        }
        if (outPipe[1] >= 0) {
            ::close(outPipe[1]);
            outPipe[1] = -1;
        }
        if (errPipe[1] >= 0) {
            ::close(errPipe[1]);
            errPipe[1] = -1;
        }
    };

    if (devNull.fd < 0) {
        result.stderr_text = "failed to open /dev/null";
        return result;
    }
    if (options.capture_stdout && makePipe(outPipe) != 0) {
        result.stderr_text = "failed to create stdout pipe";
        return result;
    }
    if (options.capture_stderr && !options.inherit_stderr && makePipe(errPipe) != 0) {
        if (outPipe[0] >= 0) {
            ::close(outPipe[0]);
        }
        if (outPipe[1] >= 0) {
            ::close(outPipe[1]);
        }
        result.stderr_text = "failed to create stderr pipe";
        return result;
    }

    if (posix_spawn_file_actions_init(&actions) != 0) {
        cleanupSpawn();
        result.stderr_text = "posix_spawn_file_actions_init failed";
        return result;
    }
    actionsInit = true;
    if (posix_spawnattr_init(&attrs) != 0) {
        cleanupSpawn();
        result.stderr_text = "posix_spawnattr_init failed";
        return result;
    }
    attrsInit = true;

    posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attrs, 0);
    posix_spawn_file_actions_adddup2(&actions, devNull.fd, STDIN_FILENO);
    if (options.capture_stdout) {
        posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, outPipe[0]);
        posix_spawn_file_actions_addclose(&actions, outPipe[1]);
    }
    if (options.capture_stderr && !options.inherit_stderr) {
        posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, errPipe[0]);
        posix_spawn_file_actions_addclose(&actions, errPipe[1]);
    }

    pid_t pid = 0;
    const int spawnRc =
        posix_spawnp(&pid, owned[0].c_str(), &actions, &attrs, ptrs.data(), environ);
    if (outPipe[1] >= 0) {
        ::close(outPipe[1]);
        outPipe[1] = -1;
    }
    if (errPipe[1] >= 0) {
        ::close(errPipe[1]);
        errPipe[1] = -1;
    }
    if (outPipe[0] >= 0) {
        outRead = Fd(outPipe[0]);
        outPipe[0] = -1;
        setNonBlocking(outRead.fd);
    }
    if (errPipe[0] >= 0) {
        errRead = Fd(errPipe[0]);
        errPipe[0] = -1;
        setNonBlocking(errRead.fd);
    }
    cleanupSpawn();

    if (spawnRc != 0) {
        result.not_found = (spawnRc == ENOENT || spawnRc == EACCES || spawnRc == ENOEXEC);
        result.exit_code = 127;
        result.stderr_text = std::strerror(spawnRc);
        return result;
    }

    ChildGuard child(pid);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeout_ms);
    bool timedOut = false;
    bool canceled = false;
    bool cancelArmed = false;
    auto cancelKillAt = std::chrono::steady_clock::time_point{};
    LineBuffer outLine;
    LineBuffer errLine;

    auto armCancel = [&]() {
        if (cancelArmed || timedOut) {
            return;
        }
        canceled = true;
        cancelArmed = true;
        killProcessGroup(pid, SIGTERM);
        cancelKillAt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    };

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const bool cancelRequested =
            g_shutdown.load() || (options.cancel && options.cancel->load());
        if (cancelRequested) {
            armCancel();
        }
        if (cancelArmed && now >= cancelKillAt) {
            killProcessGroup(pid, SIGKILL);
        }
        if (!timedOut && !cancelArmed && now >= deadline) {
            timedOut = true;
            killProcessGroup(pid, SIGKILL);
        }

        pollfd fds[2]{};
        nfds_t nfds = 0;
        int outIndex = -1;
        int errIndex = -1;
        if (outRead.fd >= 0) {
            outIndex = static_cast<int>(nfds);
            fds[nfds].fd = outRead.fd;
            fds[nfds].events = POLLIN;
            ++nfds;
        }
        if (errRead.fd >= 0) {
            errIndex = static_cast<int>(nfds);
            fds[nfds].fd = errRead.fd;
            fds[nfds].events = POLLIN;
            ++nfds;
        }
        if (nfds > 0) {
            ::poll(fds, nfds, 100);
            char chunk[4096];
            if (outIndex >= 0 && (fds[outIndex].revents & (POLLIN | POLLHUP | POLLERR))) {
                const ssize_t n = ::read(outRead.fd, chunk, sizeof(chunk));
                if (n > 0) {
                    consumeChunk(result.stdout_text, outLine, chunk, static_cast<std::size_t>(n),
                                 options, result);
                } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                    outRead.close();
                }
            }
            if (errIndex >= 0 && (fds[errIndex].revents & (POLLIN | POLLHUP | POLLERR))) {
                const ssize_t n = ::read(errRead.fd, chunk, sizeof(chunk));
                if (n > 0) {
                    consumeChunk(result.stderr_text, errLine, chunk, static_cast<std::size_t>(n),
                                 options, result);
                } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                    errRead.close();
                }
            }
        }

        int status = 0;
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            child.markReaped();
            if (outRead.fd >= 0) {
                drainFd(outRead.fd, result.stdout_text, outLine, options, result);
            }
            if (errRead.fd >= 0) {
                drainFd(errRead.fd, result.stderr_text, errLine, options, result);
            }
            flushPending(outLine, options);
            flushPending(errLine, options);
            if (WIFEXITED(status)) {
                result.exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                result.exit_code = 128 + WTERMSIG(status);
            }
            break;
        }
        if (nfds == 0) {
            ::usleep(20000);
        }
    }

    result.timed_out = timedOut;
    result.canceled = canceled && !timedOut;
    return result;
}
#else

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), needed);
    if (!out.empty() && out.back() == L'\0') {
        out.pop_back();
    }
    return out;
}

std::wstring quoteWindowsArg(const std::string& arg) {
    std::wstring wide = utf8ToWide(arg);
    bool needsQuotes = wide.empty();
    for (wchar_t ch : wide) {
        if (ch == L' ' || ch == L'\t' || ch == L'"') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) {
        return wide;
    }
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : wide) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            slashes = 0;
            continue;
        }
        if (slashes > 0) {
            quoted.append(slashes, L'\\');
            slashes = 0;
        }
        quoted.push_back(ch);
    }
    if (slashes > 0) {
        quoted.append(slashes * 2, L'\\');
    }
    quoted.push_back(L'"');
    return quoted;
}

void registerChild(HANDLE process) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_children.insert(reinterpret_cast<long long>(process));
}

void unregisterChild(HANDLE process) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_children.erase(reinterpret_cast<long long>(process));
}

RunResult runWindows(const std::vector<std::string>& argv, const RunOptions& options) {
    RunResult result;
    std::wstring command;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) {
            command.push_back(L' ');
        }
        command += quoteWindowsArg(argv[i]);
    }
    std::vector<wchar_t> mutableCmd(command.begin(), command.end());
    mutableCmd.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    HANDLE errRead = nullptr;
    HANDLE errWrite = nullptr;
    if (options.capture_stdout) {
        CreatePipe(&outRead, &outWrite, &sa, 0);
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    }
    if (options.capture_stderr && !options.inherit_stderr) {
        CreatePipe(&errRead, &errWrite, &sa, 0);
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = options.capture_stdout ? outWrite : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = (options.capture_stderr && !options.inherit_stderr)
                       ? errWrite
                       : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    const BOOL created =
        CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                       CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (outWrite) {
        CloseHandle(outWrite);
    }
    if (errWrite) {
        CloseHandle(errWrite);
    }
    if (!created) {
        result.not_found = true;
        result.exit_code = 127;
        return result;
    }
    CloseHandle(pi.hThread);
    registerChild(pi.hProcess);

    // Releases the process and pipe handles on every exit path. If run() unwinds before the child
    // has exited (for example a throwing line callback), the child is terminated first.
    struct WinChildGuard {
        HANDLE process;
        HANDLE& out;
        HANDLE& err;
        bool exited = false;
        ~WinChildGuard() {
            if (!exited) {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 5000);
            }
            if (out) {
                CloseHandle(out);
                out = nullptr;
            }
            if (err) {
                CloseHandle(err);
                err = nullptr;
            }
            unregisterChild(process);
            CloseHandle(process);
        }
    } guard{pi.hProcess, outRead, errRead};

    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(options.timeout_ms < 0 ? 86400000 : options.timeout_ms);
    bool cancelArmed = false;
    auto cancelKillAt = std::chrono::steady_clock::time_point{};
    LineBuffer outLine;
    LineBuffer errLine;
    DWORD waited = WAIT_TIMEOUT;

    auto pump = [&](HANDLE handle, std::string& dest, LineBuffer& line) {
        if (!handle) {
            return;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
            return;
        }
        while (available > 0) {
            char chunk[4096];
            const DWORD toRead =
                available > sizeof(chunk) ? static_cast<DWORD>(sizeof(chunk)) : available;
            DWORD n = 0;
            if (!ReadFile(handle, chunk, toRead, &n, nullptr) || n == 0) {
                break;
            }
            consumeChunk(dest, line, chunk, n, options, result);
            available -= n;
        }
    };

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const bool cancelRequested =
            g_shutdown.load() || (options.cancel && options.cancel->load());
        if (cancelRequested && !cancelArmed && !result.timed_out) {
            result.canceled = true;
            cancelArmed = true;
            cancelKillAt = now + std::chrono::seconds(2);
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi.dwProcessId);
        }
        if ((cancelArmed && now >= cancelKillAt) ||
            (!result.timed_out && !cancelArmed && now >= deadline)) {
            if (!cancelArmed && now >= deadline) {
                result.timed_out = true;
            }
            TerminateProcess(pi.hProcess, 1);
        }
        pump(outRead, result.stdout_text, outLine);
        pump(errRead, result.stderr_text, errLine);
        waited = WaitForSingleObject(pi.hProcess, 50);
        if (waited == WAIT_OBJECT_0) {
            guard.exited = true;
            pump(outRead, result.stdout_text, outLine);
            pump(errRead, result.stderr_text, errLine);
            flushPending(outLine, options);
            flushPending(errLine, options);
            break;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = static_cast<int>(code);
    return result;
}

#endif

} // namespace

RunResult run(const std::vector<std::string>& argv, const RunOptions& options) {
    RunResult result;
    if (argv.empty() || argv[0].empty()) {
        result.not_found = true;
        result.exit_code = 127;
        result.stderr_text = "empty argv";
        return result;
    }
    if (g_shutdown.load()) {
        result.canceled = true;
        result.exit_code = 130;
        return result;
    }
#ifndef _WIN32
    return runPosix(argv, options);
#else
    return runWindows(argv, options);
#endif
}

void requestShutdown() {
    // Async-signal-safe. The wait loop sends SIGTERM, then SIGKILL after 2 seconds.
    g_shutdown.store(true);
}

bool shutdownRequested() {
    return g_shutdown.load();
}

void resetShutdownForTests() {
    g_shutdown.store(false);
}

} // namespace yt::process
