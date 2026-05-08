#pragma once

#include <vector>
#include <unordered_map>
#include <cstdint>
#include <array>
#include <opencv2/opencv.hpp>
#include "BoardLocalizer.h"

namespace cta {

class RevertManager {
public:
    RevertManager(const BoardGeometry& geo, int margin_h, int margin_w);

    void initialize(const cv::Mat& initial_board_gray, const std::vector<double>& initial_hash);
    void push_state(const cv::Mat& board_gray, const std::vector<double>& hash);
    void resize_history(int new_size);

    int find_revert_idx(const cv::Mat& board_gray,
                        const std::vector<double>& current_hash,
                        long long& revert_hash_tests,
                        long long& revert_full_tests,
                        long long& revert_index_queries,
                        long long& revert_index_fallbacks,
                        bool exhaustive_fallback);

    const cv::Mat& get_latest_gray() const { return board_image_history_.back(); }
    size_t history_size() const { return board_image_history_.size(); }

private:
    BoardGeometry geo_;
    int margin_h_;
    int margin_w_;

    std::vector<cv::Mat> board_image_history_;
    std::vector<std::vector<double>> history_hashes_;
    std::unordered_map<std::uint64_t, std::vector<int>> history_hash_index_;
    std::vector<int> revert_test_generation_;
    int revert_generation_ = 0;

    static constexpr double kRevertMaxSquareHashDiff = 15.0;
    static constexpr double kRevertMeanHashDiff = 8.0;
    static constexpr double kRevertFullImageMeanDiff = 3.0;

    void rebuild_history_hash_index();
    std::vector<std::uint64_t> make_revert_index_keys(const std::vector<double>& hash) const;
    std::uint64_t exact_revert_index_key(const std::vector<double>& hash) const;
    int revert_bucket(double v) const;
    std::uint64_t pack_revert_key(const std::array<int, 4>& buckets) const;
    std::array<int, 4> quadrant_revert_buckets(const std::vector<double>& hash) const;
};

} // namespace cta