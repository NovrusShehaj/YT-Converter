#ifndef YT_CONVERTER_API_APP_H
#define YT_CONVERTER_API_APP_H

#include "config.h"

#include <memory>
#include <string>

namespace yt::api {

class ApiServer {
public:
    explicit ApiServer(Config config);
    ~ApiServer();

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    void start();
    void stop();
    void runUntilSignal();
    bool isRunning() const;
    std::string listenUrl() const;
    int port() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

void requestApiShutdown();
bool apiShutdownRequested();

} // namespace yt::api

#endif // YT_CONVERTER_API_APP_H
