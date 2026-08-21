#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <queue>
#include <unordered_map>
#include <vector>

namespace cta {

struct LichessGameMetadata {
    bool found = false;
    std::string id;
    std::string event;
    std::string site;
    std::string date;
    std::string round;
    std::string white;
    std::string black;
    std::string result;
    std::string white_elo;
    std::string black_elo;
    std::string eco;
    std::string opening;
    int http_status = 0;
    std::string error;
};

struct LichessOpening {
    std::string eco;
    std::string name;
    std::string error;
    long long total_games = 0;
    int http_status = 0;
    unsigned long winhttp_error = 0;
    bool found = false;
    bool api_success = false;
    std::vector<std::string> top_games;
    std::vector<std::string> top_game_ids;
    LichessGameMetadata game_metadata;
};

class OpeningFetcher {
public:
    OpeningFetcher();
    ~OpeningFetcher();

    void enqueue_fen(const std::string& fen);
    LichessOpening get_opening(const std::string& fen);
    void resolve_game_metadata(
        const std::vector<std::string>& observed_fens,
        const std::vector<std::string>& observed_moves = {});
    void wait_until_done();
    bool wait_until_done_for(std::chrono::milliseconds timeout);
    std::string metadata_resolution_error();
    void set_api_token(const std::string& token);
    bool test_connection(std::string& out_error);

private:
    void worker_thread();
    LichessOpening fetch_from_lichess(
        const std::string& fen,
        const std::string& play = {});
    LichessGameMetadata fetch_master_game(const std::string& game_id);

    std::thread thread_;
    std::queue<std::string> queue_;
    std::unordered_map<std::string, LichessOpening> cache_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_;
    std::atomic<bool> unique_game_reached_;
    std::atomic<bool> done_;
    bool active_request_ = false;
    std::string cache_path_;
    std::string api_token_;
    std::string metadata_resolution_error_;
    std::unordered_map<std::string, std::vector<std::string>> master_game_fens_;
};

} // namespace cta
