#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_map>

namespace cta {

struct LichessOpening {
    std::string eco;
    std::string name;
    int total_games = 0;
    bool found = false;
    bool api_success = false;
};

class OpeningFetcher {
public:
    OpeningFetcher();
    ~OpeningFetcher();

    void enqueue_fen(const std::string& fen);
    LichessOpening get_opening(const std::string& fen);
    void wait_until_done();

private:
    void worker_thread();
    LichessOpening fetch_from_lichess(const std::string& fen);

    std::thread thread_;
    std::queue<std::string> queue_;
    std::unordered_map<std::string, LichessOpening> cache_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_;
    std::atomic<bool> unique_game_reached_;
    std::atomic<bool> done_;
    std::string cache_path_;
};

} // namespace cta