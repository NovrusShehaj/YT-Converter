// Portable child-process fixture for process tests. Each argument is one action, run in order:
//   out:N:C      write N copies of byte C to stdout (no newline)
//   err:N:C      write N copies of byte C to stderr (no newline)
//   outline:TEXT write TEXT and a newline to stdout
//   errline:TEXT write TEXT and a newline to stderr
//   spam:C       write byte C to stdout forever (until killed)
//   sleep:MS     sleep for MS milliseconds
//   pidfile:PATH write this process ID to PATH
//   exit:CODE    exit immediately with CODE
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define YTCONV_GETPID _getpid
#else
#include <unistd.h>
#define YTCONV_GETPID getpid
#endif

namespace {

void writeRepeated(std::FILE* stream, std::size_t count, char byte) {
    std::vector<char> block(65536, byte);
    while (count > 0) {
        const std::size_t n = count < block.size() ? count : block.size();
        if (std::fwrite(block.data(), 1, n, stream) != n) {
            std::exit(3);
        }
        count -= n;
    }
    std::fflush(stream);
}

} // namespace

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        const std::string action = argv[i];
        const auto colon = action.find(':');
        const std::string verb = action.substr(0, colon);
        const std::string rest = colon == std::string::npos ? "" : action.substr(colon + 1);
        if (verb == "out" || verb == "err") {
            const auto second = rest.find(':');
            const std::size_t count = std::stoull(rest.substr(0, second));
            const char byte = second == std::string::npos ? 'x' : rest[second + 1];
            writeRepeated(verb == "out" ? stdout : stderr, count, byte);
        } else if (verb == "outline" || verb == "errline") {
            std::FILE* stream = verb == "outline" ? stdout : stderr;
            std::fputs(rest.c_str(), stream);
            std::fputc('\n', stream);
            std::fflush(stream);
        } else if (verb == "spam") {
            const char byte = rest.empty() ? 'x' : rest[0];
            while (true) {
                writeRepeated(stdout, 65536, byte);
            }
        } else if (verb == "sleep") {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::stoll(rest)));
        } else if (verb == "pidfile") {
            std::ofstream(rest) << YTCONV_GETPID() << '\n';
        } else if (verb == "exit") {
            return std::atoi(rest.c_str());
        } else {
            std::fprintf(stderr, "unknown action: %s\n", action.c_str());
            return 2;
        }
    }
    return 0;
}
