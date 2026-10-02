#ifndef YT_CONVERTER_TEST_HELPERS_H
#define YT_CONVERTER_TEST_HELPERS_H

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <thread>
#include <sstream>
#include <string>
#include <vector>

#ifndef YTCONV_FAKE_DIR
#define YTCONV_FAKE_DIR ""
#endif

inline std::string testRandomName() {
    std::random_device device;
    std::mt19937_64 rng(device());
    std::ostringstream ss;
    ss << std::hex << rng();
    return ss.str();
}

inline std::filesystem::path makeTestDir() {
    auto path = std::filesystem::temp_directory_path() / "ytconv-tests" / testRandomName();
    std::filesystem::create_directories(path);
    return path;
}

inline std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline std::string fakePath(const std::string& name) {
    return (std::filesystem::path(YTCONV_FAKE_DIR) / name).string();
}

inline std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

inline void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

// Counts leftover temporary outputs anywhere under the output root.
inline int countPartialFiles(const std::filesystem::path& root) {
    int count = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        if (it->path().extension() == ".partial") {
            ++count;
        }
    }
    return count;
}

// Polls a condition until it holds or the timeout passes. Used for barriers, not as a race guess.
inline bool waitUntil(const std::function<bool()>& condition,
                      std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// PIDs of gated fakes that touched "started.<pid>" in a gate directory.
inline std::vector<long> gateStartedPids(const std::filesystem::path& gate) {
    std::vector<long> pids;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(gate, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("started.", 0) == 0) {
            try {
                pids.push_back(std::stol(name.substr(8)));
            } catch (const std::exception&) {
            }
        }
    }
    return pids;
}

inline void openGate(const std::filesystem::path& gate) {
    writeFile(gate / "release", "");
}

#endif
