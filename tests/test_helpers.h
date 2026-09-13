#ifndef YT_CONVERTER_TEST_HELPERS_H
#define YT_CONVERTER_TEST_HELPERS_H

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

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

#endif
