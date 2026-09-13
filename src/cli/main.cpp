#include "config.h"
#include "converter.h"
#include "dependencies.h"
#include "error.h"
#include "logger.h"
#include "process.h"
#include "validation.h"
#include "version.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void handleSignal(int) { yt::process::requestShutdown(); }

void printUsage(const char* programName, bool unicode) {
    const char* dash = unicode ? "—" : "-";
    std::cout << "YouTube Converter CLI " << YTCONV_VERSION << "\n\n"
              << "Usage:\n"
              << "  " << programName << " [options] <URL> [format]\n\n"
              << "Arguments:\n"
              << "  URL                 YouTube watch, shorts, embed, live, or youtu.be URL\n"
              << "  format              Output format: mp3, mp4, or wav (default: mp3)\n\n"
              << "Options:\n"
              << "  -h, --help          Show this help and exit\n"
              << "  -V, --version       Print version and exit\n"
              << "  -o, --output-dir D  Directory for finished files (default: ./output)\n"
              << "  -f, --format FMT    Output format\n"
              << "      --quality N     Max video height for mp4 (default: 1080)\n"
              << "      --force         Replace an existing output file\n"
              << "      --no-unicode    Use ASCII status markers\n"
              << "  -v, --verbose       Debug logging\n"
              << "  -q, --quiet         Log errors only\n\n"
              << "Exit codes:\n"
              << "  0 success, 2 validation, 3 missing tools, 4 download, 5 convert, 130 canceled\n\n"
              << "This tool is for personal, localhost use. You are responsible for YouTube\n"
              << "Terms of Service and copyright compliance. " << dash
              << " Public internet hosting is out of scope.\n";
}

void printVersion() { std::cout << "yt2mp3-cli " << YTCONV_VERSION << '\n'; }

} // namespace

int main(int argc, char* argv[]) {
    yt::Config config = yt::loadConfigFromEnv();
    std::vector<std::string> positionals;
    std::string formatOverride;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << '\n';
                printUsage(argv[0], !config.no_unicode);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0], !config.no_unicode);
            return 0;
        }
        if (arg == "--version" || arg == "-V") {
            printVersion();
            return 0;
        }
        if (arg == "--output-dir" || arg == "-o") {
            config.output_dir = requireValue(arg);
        } else if (arg == "--format" || arg == "-f") {
            formatOverride = requireValue(arg);
        } else if (arg == "--quality") {
            try {
                config.max_height = std::stoi(requireValue(arg));
            } catch (const std::exception&) {
                std::cerr << "Invalid --quality value\n";
                return 2;
            }
        } else if (arg == "--force") {
            config.force = true;
        } else if (arg == "--no-unicode") {
            config.no_unicode = true;
        } else if (arg == "--verbose" || arg == "-v") {
            config.log_level = "DEBUG";
        } else if (arg == "--quiet" || arg == "-q") {
            config.log_level = "ERROR";
        } else if (arg.rfind("--", 0) == 0 || (!arg.empty() && arg[0] == '-' && arg != "-")) {
            std::cerr << "Unknown option: " << arg << '\n';
            printUsage(argv[0], !config.no_unicode);
            return 2;
        } else {
            positionals.push_back(arg);
        }
    }

    if (positionals.empty()) {
        std::cerr << "Missing YouTube URL\n";
        printUsage(argv[0], !config.no_unicode);
        return 2;
    }
    if (positionals.size() > 2) {
        std::cerr << "Too many arguments\n";
        printUsage(argv[0], !config.no_unicode);
        return 2;
    }

    const std::string url = positionals[0];
    std::string format = formatOverride;
    if (positionals.size() == 2) {
        format = positionals[1];
    }
    if (format.empty()) {
        format = "mp3";
    }

    yt::applyLogConfig(config);
    auto& logger = yt::logger::Logger::getInstance();

#ifndef _WIN32
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
#endif

    try {
        yt::validation::requireVideo(url);
        yt::validation::requireFormat(format);
        yt::deps::requireTools(config);
        yt::converter::ConversionRequest request;
        request.url = url;
        request.format = format;
        request.config = config;
        request.show_progress = config.log_level != "ERROR";
        const auto result = yt::converter::processVideo(request);

        const char* okMark = config.no_unicode ? "OK" : "\xE2\x9C\x93";
        std::cout << okMark << " Conversion completed\n"
                  << "Output: " << result.output_path << '\n';
        return 0;
    } catch (const yt::Error& error) {
        logger.error(error.message());
        const char* badMark = config.no_unicode ? "ERROR" : "\xE2\x9C\x97";
        std::cerr << badMark << ' ' << error.message() << '\n';
        if (error.code() == yt::ErrorCode::BinaryNotFound) {
            std::cerr << yt::deps::installHint() << '\n';
        }
        return error.exitCode();
    } catch (const std::exception& error) {
        logger.critical(error.what());
        std::cerr << "Unexpected error\n";
        return 1;
    }
}
