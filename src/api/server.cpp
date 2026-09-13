#include "api_app.h"
#include "config.h"
#include "dependencies.h"
#include "error.h"
#include "logger.h"
#include "process.h"
#include "version.h"

#include <iostream>
#include <string>

namespace {

void printServerInfo(const yt::Config& config, bool unauthenticated) {
    std::cout << "YouTube Converter API " << YTCONV_VERSION << '\n'
              << "Listening on http://" << config.bind << ':' << config.port << '\n'
              << "Endpoints:\n"
              << "  POST /v1/conversions\n"
              << "  GET  /v1/healthz\n"
              << "  GET  /v1/readyz\n"
              << "  GET  /v1/metrics   (loopback only)\n";
    if (unauthenticated) {
        std::cout << "Authentication: disabled (localhost only)\n";
    } else {
        std::cout << "Authentication: X-Api-Key required\n";
    }
    std::cout << "Bind is loopback by default. Do not expose this service on the public internet.\n";
}

} // namespace

int main(int argc, char* argv[]) {
    yt::Config config = yt::loadConfigFromEnv();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto requireValue = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << '\n';
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            std::cout << "yt2mp3-api " << YTCONV_VERSION << "\n"
                      << "Usage: " << argv[0] << " [options]\n"
                      << "  --allow-unauthenticated-localhost\n"
                      << "  --bind ADDRESS          default 127.0.0.1\n"
                      << "  --port N                default 8080\n"
                      << "  --output-dir DIR        default ./output\n"
                      << "  --help, --version\n";
            return 0;
        }
        if (arg == "--version" || arg == "-V") {
            std::cout << "yt2mp3-api " << YTCONV_VERSION << '\n';
            return 0;
        }
        if (arg == "--allow-unauthenticated-localhost") {
            config.allow_unauthenticated_localhost = true;
        } else if (arg == "--bind") {
            config.bind = requireValue(arg);
        } else if (arg == "--port") {
            try {
                config.port = std::stoi(requireValue(arg));
            } catch (const std::exception&) {
                std::cerr << "Invalid --port value\n";
                return 2;
            }
        } else if (arg == "--output-dir") {
            config.output_dir = requireValue(arg);
        } else {
            std::cerr << "Unknown option: " << arg << '\n';
            return 2;
        }
    }

    try {
        yt::validateApiConfig(config);
        yt::applyLogConfig(config);
        auto& logger = yt::logger::Logger::getInstance();
        logger.info("Starting yt2mp3-api " YTCONV_VERSION);
        const auto tools = yt::deps::checkTools(config);
        if (!tools.ok) {
            logger.warning("Readiness will fail until tools are installed: " + tools.message);
        }
        printServerInfo(config, config.api_key.empty());
        yt::api::ApiServer server(config);
        server.runUntilSignal();
        yt::process::resetShutdownForTests();
        return 0;
    } catch (const yt::Error& error) {
        std::cerr << error.message() << '\n';
        return error.exitCode();
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
