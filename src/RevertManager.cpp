#include "RevertManager.h"
#include "BoardAnalysis.h"
#include "GPUAccelerator.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace cta {

RevertManager::RevertManager(const BoardGeometry& geo, int margin_h, int margin_w)
    : geo_(geo), margin_h_(margin_h), margin_w_(margin_w) {
}

void RevertManager::initialize(const cv::Mat& initial_board_gray, const std::vector<double>& initial_hash) {
    board_image_history_.clear();
    history_hashes_.clear();
    history_hash_index_.clear();
    
    board_image_history_.push_back(initial_board_gray.clone());
    history_hashes_.push_back(initial_hash);
    rebuild_history_hash_index();
}

void RevertManager::push_state(const cv::Mat& board_gray, const std::vector<double>& hash) {
    board_image_history_.push_back(board_gray.clone());
    history_hashes_.push_back(hash);
    int history_idx = static_cast<int>(history_hashes_.size()) - 1;
    for (std::uint64_t key : make_revert_index_keys(history_hashes_.back())) {
        history_hash_index_[key].push_back(history_idx);
    }
}

void RevertManager::resize_history(int new_size) {
    board_image_history_.resize(new_size);
    history_hashes_.resize(new_size);
    rebuild_history_hash_index();
}

int RevertManager::revert_bucket(double v) const {
    int q = static_cast<int>(std::round(v / 8.0));
    return std::clamp(q, 0, 63);
}

std::uint64_t RevertManager::pack_revert_key(const std::array<int, 4>& buckets) const {
    return (static_cast<std::uint64_t>(buckets[0])      ) |
           (static_cast<std::uint64_t>(buckets[1]) <<  6) |
           (static_cast<std::uint64_t>(buckets[2]) << 12) |
           (static_cast<std::uint64_t>(buckets[3]) << 18);
}

std::array<int, 4> RevertManager::quadrant_revert_buckets(const std::vector<double>& hash) const {
    std::array<double, 4> totals{0.0, 0.0, 0.0, 0.0};
    std::array<int, 4> counts{0, 0, 0, 0};
    for (int sq = 0; sq < 64; ++sq) {
        int row = sq / 8;
        int col = sq % 8;
        int q = (row >= 4 ? 2 : 0) + (col >= 4 ? 1 : 0);
        totals[q] += hash[sq];
        ++counts[q];
    }

    std::array<int, 4> buckets{};
    for (int q = 0; q < 4; ++q) {
        buckets[q] = revert_bucket(totals[q] / std::max(1, counts[q]));
    }
    return buckets;
}

std::vector<std::uint64_t> RevertManager::make_revert_index_keys(const std::vector<double>& hash) const {
    std::vector<std::uint64_t> keys;
    keys.reserve(81);
    std::array<int, 4> center = quadrant_revert_buckets(hash);
    for (int d0 = -1; d0 <= 1; ++d0) {
        for (int d1 = -1; d1 <= 1; ++d1) {
            for (int d2 = -1; d2 <= 1; ++d2) {
                for (int d3 = -1; d3 <= 1; ++d3) {
                    std::array<int, 4> key_buckets{
                        std::clamp(center[0] + d0, 0, 63),
                        std::clamp(center[1] + d1, 0, 63),
                        std::clamp(center[2] + d2, 0, 63),
                        std::clamp(center[3] + d3, 0, 63)
                    };
                    keys.push_back(pack_revert_key(key_buckets));
                }
            }
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

std::uint64_t RevertManager::exact_revert_index_key(const std::vector<double>& hash) const {
    return pack_revert_key(quadrant_revert_buckets(hash));
}

void RevertManager::rebuild_history_hash_index() {
    history_hash_index_.clear();
    history_hash_index_.reserve(history_hashes_.size() * 96 + 1);
    for (int idx = 0; idx < static_cast<int>(history_hashes_.size()); ++idx) {
        for (std::uint64_t key : make_revert_index_keys(history_hashes_[idx])) {
            history_hash_index_[key].push_back(idx);
        }
    }
}

int RevertManager::find_revert_idx(const cv::Mat& board_gray,
                                   const std::vector<double>& current_hash,
                                   long long& revert_hash_tests,
                                   long long& revert_full_tests,
                                   long long& revert_index_queries,
                                   long long& revert_index_fallbacks,
                                   bool exhaustive_fallback) {
    int best_idx = -1;
    double best_diff_val = 1e18;

    auto test_revert_candidate = [&](int idx) {
        if (idx < 0 || idx >= static_cast<int>(history_hashes_.size()) - 1) return;
        ++revert_hash_tests;
        double max_hash_diff = 0.0, sum_hash_diff = 0.0;
        for (int i = 0; i < 64; ++i) {
            double d_val = std::abs(current_hash[i] - history_hashes_[idx][i]);
            sum_hash_diff += d_val;
            if (d_val > max_hash_diff) max_hash_diff = d_val;
            if (max_hash_diff >= kRevertMaxSquareHashDiff) break;
        }
        if (max_hash_diff >= kRevertMaxSquareHashDiff || (sum_hash_diff / 64.0) >= kRevertMeanHashDiff) return;
        ++revert_full_tests;
        static thread_local cv::Mat d;
        GPUAccelerator::absdiff(board_gray, board_image_history_[idx], d);
        double mean_diff = cv::mean(d)[0];
        if (mean_diff < best_diff_val) { best_diff_val = mean_diff; best_idx = idx; }
    };

    ++revert_index_queries;
    auto indexed_it = history_hash_index_.find(exact_revert_index_key(current_hash));
    if (indexed_it != history_hash_index_.end()) {
        ++revert_generation_;
        if (revert_generation_ == std::numeric_limits<int>::max()) { std::fill(revert_test_generation_.begin(), revert_test_generation_.end(), 0); revert_generation_ = 1; }
        if (revert_test_generation_.size() < history_hashes_.size()) revert_test_generation_.resize(history_hashes_.size(), 0);
        for (auto it = indexed_it->second.rbegin(); it != indexed_it->second.rend(); ++it) {
            int idx = *it; if (idx >= 0 && idx < static_cast<int>(revert_test_generation_.size()) && revert_test_generation_[idx] != revert_generation_) { revert_test_generation_[idx] = revert_generation_; test_revert_candidate(idx); }
        }
    }
    if (exhaustive_fallback && (best_idx < 0 || best_diff_val >= kRevertFullImageMeanDiff)) {
        ++revert_index_fallbacks;
        for (int idx = static_cast<int>(history_hashes_.size()) - 2; idx >= 0; --idx) test_revert_candidate(idx);
    }
    return (best_idx >= 0 && best_diff_val < kRevertFullImageMeanDiff) ? best_idx : -1;
}

} // namespace cta