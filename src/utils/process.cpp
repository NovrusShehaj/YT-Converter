#include "process.h"

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

extern char** environ;

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

void drainFd(int fd, std::string& buffer, std::size_t maxBytes) {
    char chunk[1024];
    while (true) {
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            appendBounded(buffer, chunk, static_cast<std::size_t>(n), maxBytes);
            continue;
        }
        break;
    }
}

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
    const int spawnRc = posix_spawnp(&pid, owned[0].c_str(), &actions, &attrs, ptrs.data(), environ);
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
    }
    if (errPipe[0] >= 0) {
        errRead = Fd(errPipe[0]);
        errPipe[0] = -1;
    }
    cleanupSpawn();

    if (spawnRc != 0) {
        result.not_found = (spawnRc == ENOENT || spawnRc == EACCES || spawnRc == ENOEXEC);
        result.exit_code = 127;
        result.stderr_text = std::strerror(spawnRc);
        return result;
    }

    registerChild(pid);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeout_ms);
    bool timedOut = false;
    bool canceled = false;

    while (true) {
        if (g_shutdown.load()) {
            canceled = true;
            killProcessGroup(pid, SIGTERM);
            killProcessGroup(pid, SIGKILL);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            killProcessGroup(pid, SIGTERM);
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
            char chunk[1024];
            if (outIndex >= 0 && (fds[outIndex].revents & POLLIN)) {
                const ssize_t n = ::read(outRead.fd, chunk, sizeof(chunk));
                if (n > 0) {
                    appendBounded(result.stdout_text, chunk, static_cast<std::size_t>(n),
                                  options.max_output_bytes);
                } else if (n == 0) {
                    outRead.close();
                }
            }
            if (errIndex >= 0 && (fds[errIndex].revents & POLLIN)) {
                const ssize_t n = ::read(errRead.fd, chunk, sizeof(chunk));
                if (n > 0) {
                    appendBounded(result.stderr_text, chunk, static_cast<std::size_t>(n),
                                  options.max_output_bytes);
                } else if (n == 0) {
                    errRead.close();
                }
            }
        }

        int status = 0;
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            if (outRead.fd >= 0) {
                drainFd(outRead.fd, result.stdout_text, options.max_output_bytes);
            }
            if (errRead.fd >= 0) {
                drainFd(errRead.fd, result.stderr_text, options.max_output_bytes);
            }
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

    unregisterChild(pid);
    result.timed_out = timedOut;
    result.canceled = canceled && !timedOut;
    return result;
}

#else

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
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

    const DWORD waitMs = options.timeout_ms < 0 ? INFINITE : static_cast<DWORD>(options.timeout_ms);
    const DWORD waited = WaitForSingleObject(pi.hProcess, waitMs);
    if (waited == WAIT_TIMEOUT || g_shutdown.load()) {
        result.timed_out = waited == WAIT_TIMEOUT;
        result.canceled = g_shutdown.load() && waited != WAIT_TIMEOUT;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = static_cast<int>(code);

    auto readHandle = [&](HANDLE handle, std::string& dest) {
        if (!handle) {
            return;
        }
        char chunk[1024];
        DWORD n = 0;
        while (ReadFile(handle, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
            appendBounded(dest, chunk, n, options.max_output_bytes);
        }
    };
    readHandle(outRead, result.stdout_text);
    readHandle(errRead, result.stderr_text);
    if (outRead) {
        CloseHandle(outRead);
    }
    if (errRead) {
        CloseHandle(errRead);
    }
    unregisterChild(pi.hProcess);
    CloseHandle(pi.hProcess);
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
    g_shutdown.store(true);
    std::lock_guard<std::mutex> lock(g_mutex);
#ifndef _WIN32
    for (long long pid : g_children) {
        killProcessGroup(static_cast<pid_t>(pid), SIGTERM);
    }
    for (long long pid : g_children) {
        killProcessGroup(static_cast<pid_t>(pid), SIGKILL);
    }
#else
    for (long long handleValue : g_children) {
        HANDLE handle = reinterpret_cast<HANDLE>(handleValue);
        TerminateProcess(handle, 1);
    }
#endif
}

bool shutdownRequested() { return g_shutdown.load(); }

void resetShutdownForTests() { g_shutdown.store(false); }

} // namespace yt::process
