#ifndef YT_CONVERTER_SINGLEFLIGHT_H
#define YT_CONVERTER_SINGLEFLIGHT_H

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace yt::singleflight {

struct Flight {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    int error_code = 0;
    std::string error;
    std::string value;
    std::uint64_t download_ms = 0;
    std::uint64_t convert_ms = 0;
    std::uint64_t bytes = 0;
};

struct Membership {
    std::shared_ptr<Flight> flight;
    bool is_leader = false;
};

class Group {
  public:
    Group() = default;
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;

    Membership join(const std::string& key);
    void succeed(const std::string& key, const std::shared_ptr<Flight>& flight);
    void fail(const std::string& key, const std::shared_ptr<Flight>& flight, int errorCode,
              std::string error);
    void wait(const std::shared_ptr<Flight>& flight);

  private:
    std::mutex mu_;
    std::map<std::string, std::shared_ptr<Flight>> inflight_;
};

} // namespace yt::singleflight

#endif // YT_CONVERTER_SINGLEFLIGHT_H
