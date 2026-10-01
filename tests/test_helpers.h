#ifndef YT_CONVERTER_TEST_HELPERS_H
#define YT_CONVERTER_TEST_HELPERS_H

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
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

#endif
