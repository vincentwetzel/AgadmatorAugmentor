// Extracted from cpp directory
#include <gtest/gtest.h>
#include <opencv2/opencv.hpp>
#include <filesystem>
#include <vector>
#include <string>
#include <set>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <nlohmann/json.hpp>
#include <libchess/position.hpp>
#include <libchess/move.hpp>
#include "BoardLocalizer.h"
#include "UIDetectors.h"
#include "ChessVideoExtractor.h"
#include "ExtractionDiagnostics.h"
#include "VideoChunkMapper.h"
#include "../src/ChessVideoExtractor_Internal.h"

// ─── Test result tracking ────────────────────────────────────────────────────
struct IntegrationTestResult {
    std::string name;
    std::string video_file;
    double video_duration_sec = 0.0;
    int plies_extracted = 0;
    int plies_expected = 0;
    bool passed = false;
    double elapsed_sec = 0.0;
    int reverts_detected = 0;
};

static std::vector<IntegrationTestResult> g_test_results;
static size_t g_invariant_failure_count = 0;

static void expect_game_data_equal(const cta::GameData& expected,
                                   const cta::GameData& actual) {
    EXPECT_EQ(expected.moves, actual.moves);
    EXPECT_EQ(expected.timestamps, actual.timestamps);
    EXPECT_EQ(expected.fens, actual.fens);
    EXPECT_EQ(expected.video_fens, actual.video_fens);
    EXPECT_EQ(expected.video_timestamps, actual.video_timestamps);
    EXPECT_EQ(expected.video_moves, actual.video_moves);
    ASSERT_EQ(expected.clocks.size(), actual.clocks.size());
    for (size_t index = 0; index < expected.clocks.size(); ++index) {
        EXPECT_EQ(expected.clocks[index].active, actual.clocks[index].active);
        EXPECT_EQ(expected.clocks[index].white_time, actual.clocks[index].white_time);
        EXPECT_EQ(expected.clocks[index].black_time, actual.clocks[index].black_time);
        EXPECT_EQ(expected.clocks[index].moved_time_observed,
                  actual.clocks[index].moved_time_observed);
        EXPECT_EQ(expected.clocks[index].moved_time_missing,
                  actual.clocks[index].moved_time_missing);
    }

    ASSERT_EQ(expected.variations.size(), actual.variations.size());
    auto expected_variation = expected.variations.begin();
    auto actual_variation = actual.variations.begin();
    for (; expected_variation != expected.variations.end();
         ++expected_variation, ++actual_variation) {
        EXPECT_EQ(expected_variation->first, actual_variation->first);
        ASSERT_EQ(expected_variation->second.size(), actual_variation->second.size());
        for (size_t index = 0; index < expected_variation->second.size(); ++index) {
            const auto& expected_line = expected_variation->second[index];
            const auto& actual_line = actual_variation->second[index];
            EXPECT_EQ(expected_line.moves, actual_line.moves);
            EXPECT_EQ(expected_line.timestamps, actual_line.timestamps);
            EXPECT_EQ(expected_line.fens, actual_line.fens);
            EXPECT_EQ(expected_line.replay_observation, actual_line.replay_observation);
            EXPECT_EQ(expected_line.scores, actual_line.scores);
            ASSERT_EQ(expected_line.clocks.size(), actual_line.clocks.size());
            for (size_t clock_index = 0; clock_index < expected_line.clocks.size(); ++clock_index) {
                EXPECT_EQ(expected_line.clocks[clock_index].active,
                          actual_line.clocks[clock_index].active);
                EXPECT_EQ(expected_line.clocks[clock_index].white_time,
                          actual_line.clocks[clock_index].white_time);
                EXPECT_EQ(expected_line.clocks[clock_index].black_time,
                          actual_line.clocks[clock_index].black_time);
            }
        }
    }
}

static void write_invariant_diagnostic(const std::string& message) {
    const char* path = std::getenv("CTA_INVARIANT_REPORT_FILE");
    if (path == nullptr || *path == '\0') return;

    nlohmann::json record = {
        {"schema_version", 1},
        {"event", "INVARIANT_FAILED"},
        {"invariant", message},
    };
    std::ofstream output(path, std::ios::out | std::ios::app);
    if (output.is_open()) {
        output << record.dump() << '\n';
    }
}

TEST(ExtractionDiagnosticsTest, ClassifiesLegacyEvents) {
    const auto accepted = cta::diagnostics::from_legacy_trace(
        7, "ACCEPT", 12.5, 9, "fen", "e2e4", 91.0, 42.0, 35.0, 39.0, "source=settled");

    EXPECT_EQ(accepted.sequence, 7u);
    EXPECT_EQ(accepted.observation_id, 0u);
    EXPECT_EQ(accepted.transition_id, 0u);
    EXPECT_EQ(accepted.event, "ACCEPT");
    EXPECT_STREQ(cta::diagnostics::to_string(accepted.phase), "reducer");
    EXPECT_STREQ(cta::diagnostics::to_string(accepted.outcome), "accepted");
    EXPECT_STREQ(cta::diagnostics::to_string(accepted.reason), "move_accepted");

    const auto restored = cta::diagnostics::from_legacy_trace(
        8, "REVERT_APPLIED", 14.0, 5, "parent_fen", "", 0.0, 0.0,
        0.0, 0.0, "restored_ply=5;reason=analysis_revert");
    EXPECT_STREQ(cta::diagnostics::to_string(restored.phase), "revert");
    EXPECT_STREQ(cta::diagnostics::to_string(restored.outcome), "recovered");
    EXPECT_STREQ(cta::diagnostics::to_string(restored.reason), "revert_applied");
}

TEST(ExtractionDiagnosticsTest, WritesStructuredJsonLine) {
    cta::diagnostics::Evidence evidence;
    evidence.mapper_chunk = 3;
    evidence.source_frame_index = 127;
    evidence.mapper_emission_reason = "motion_leading_edge";
    evidence.diagnostic_frame_path = "frames/observation_42_frame.png";
    evidence.diagnostic_board_path = "frames/observation_42_board.png";
    evidence.diagnostic_clock_top_path = "frames/observation_42_clock_top.png";
    evidence.diagnostic_clock_bottom_path = "frames/observation_42_clock_bottom.png";
    evidence.observation_tags = {"motion", "highlight_activity", "hover", "animation"};
    evidence.yellow_arrows_checked = true;
    evidence.yellow_arrows = {"e2e4", "f1b5"};
    evidence.red_squares_checked = true;
    evidence.red_squares = {"e4"};
    evidence.template_identity = 0x123456789abcdef0ull;
    evidence.board_x = 100;
    evidence.board_y = 50;
    evidence.board_width = 800;
    evidence.board_height = 800;
    evidence.square_width = 100.0;
    evidence.square_height = 100.0;
    evidence.localization_score = 0.97;
    evidence.localization_scale = 1.25;
    evidence.board_hash = {1.0, 2.0, 3.0, 4.0};
    evidence.geometry_checked = true;
    evidence.geometry_anomaly = true;
    evidence.geometry_drift_x = 12.0;
    evidence.geometry_drift_y = -3.0;
    evidence.geometry_size_drift = 4.0;
    evidence.geometry_step_drift_x = 13.0;
    evidence.geometry_step_drift_y = -2.0;
    evidence.geometry_step_size_drift = 5.0;
    evidence.geometry_relocalization_score = 0.91;
    evidence.geometry_decision = "jump_detected";
    evidence.changed_squares.push_back({"e4", 54.0, 1});
    evidence.yellow_candidates.push_back({"d4", 78.0, 1});
    evidence.yellow_endpoint_threshold = 25.0;
    evidence.yellow_pair_threshold = 70.0;
    evidence.yellow_measurements.push_back({
        "d4",
        {32.0, 35.0, 30.0, 37.0},
        {{{80.0, 150.0, 180.0}, {82.0, 151.0, 181.0},
          {79.0, 149.0, 179.0}, {81.0, 152.0, 182.0}}},
        {0.10, 0.12, 0.08, 0.11},
        33.5});
    evidence.yellow_temporal_checked = true;
    evidence.yellow_temporal_window_seconds = 0.75;
    evidence.yellow_temporal_sample_count = 3;
    evidence.yellow_temporal_pair_pass_count = 2;
    evidence.yellow_temporal_max_from = 42.0;
    evidence.yellow_temporal_max_to = 48.0;
    evidence.yellow_temporal_max_pair = 90.0;
    evidence.yellow_assessment.state = "passed";
    evidence.yellow_assessment.thresholds = {25.0, 70.0};
    evidence.yellow_assessment.measurements = {
        {"from_score", 42.0}, {"to_score", 48.0}, {"pair_score", 90.0}};
    evidence.yellow_assessment.uncertainty_reason = "uncalibrated_detector_confidence";
    evidence.score_margin = 12.5;
    evidence.rejection_reason = "hover_box";
    evidence.hover_measurements.push_back({"e2", 0.7, 0.2, 0.8, 0.1, 0.8, 3, true});
    evidence.clock_top_width = 120;
    evidence.clock_top_height = 24;
    evidence.clock_bottom_width = 120;
    evidence.clock_bottom_height = 24;
    evidence.clock_top_bright_ratio = 0.15;
    evidence.clock_bottom_bright_ratio = 0.42;
    evidence.clock_bright_ratio_delta = 0.27;
    evidence.score_from_square_diff = 42.0;
    evidence.score_to_square_diff = 51.0;
    evidence.score_adjustment = -10.0;
    evidence.minimum_score_threshold = 35.0;
    evidence.score_threshold_checked = true;
    evidence.score_threshold_passed = true;
    evidence.score_threshold_decision = "passed";
    evidence.yellow_decision = "passed";
    evidence.hover_decision = "clear";
    evidence.clock_decision = "ocr_plausible";
    evidence.settle_decision = "accepted_same_move";
    evidence.legal_candidates.push_back({"e2e4", 91.5, 1});
    evidence.legal_candidates.push_back({"e2e3", 47.2, 2});
    const auto rejected = cta::diagnostics::from_legacy_trace(
        3, "VALIDATION_REJECTED", 4.25, 2, "fen", "", 0.0, 18.0, 0.0, 0.0,
        "reason=hover", 0, 0, evidence, 11, 5, 2, 1, "recovering");
    std::ostringstream output;
    cta::diagnostics::write_json_line(output, rejected);

    const auto json = nlohmann::json::parse(output.str());
    EXPECT_EQ(json.at("schema_version"), 1);
    EXPECT_EQ(json.at("sequence"), 3);
    EXPECT_EQ(json.at("observation_id"), 0);
    EXPECT_EQ(json.at("candidate_id"), 11);
    EXPECT_EQ(json.at("transition_id"), 0);
    EXPECT_EQ(json.at("state_generation"), 5);
    EXPECT_EQ(json.at("revert_generation"), 2);
    EXPECT_EQ(json.at("branch_id"), 1);
    EXPECT_EQ(json.at("event"), "VALIDATION_REJECTED");
    EXPECT_EQ(json.at("phase"), "validation");
    EXPECT_EQ(json.at("outcome"), "rejected");
    EXPECT_EQ(json.at("reason"), "validation_rejected");
    EXPECT_EQ(json.at("reducer_state"), "recovering");
    EXPECT_EQ(json.at("evidence").at("board_x"), 100);
    EXPECT_EQ(json.at("evidence").at("mapper_chunk"), 3);
    EXPECT_EQ(json.at("evidence").at("source_frame_index"), 127);
    EXPECT_EQ(json.at("evidence").at("mapper_emission_reason"), "motion_leading_edge");
    EXPECT_EQ(json.at("evidence").at("diagnostic_frame_path"), "frames/observation_42_frame.png");
    EXPECT_EQ(json.at("evidence").at("diagnostic_board_path"), "frames/observation_42_board.png");
    EXPECT_EQ(json.at("evidence").at("diagnostic_clock_top_path"), "frames/observation_42_clock_top.png");
    EXPECT_EQ(json.at("evidence").at("diagnostic_clock_bottom_path"), "frames/observation_42_clock_bottom.png");
    ASSERT_EQ(json.at("evidence").at("observation_tags").size(), 4u);
    EXPECT_EQ(json.at("evidence").at("observation_tags").at(0), "motion");
    EXPECT_TRUE(json.at("evidence").at("yellow_arrows_checked"));
    ASSERT_EQ(json.at("evidence").at("yellow_arrows").size(), 2u);
    EXPECT_EQ(json.at("evidence").at("yellow_arrows").at(1), "f1b5");
    EXPECT_TRUE(json.at("evidence").at("red_squares_checked"));
    EXPECT_EQ(json.at("evidence").at("red_squares").at(0), "e4");
    EXPECT_EQ(json.at("evidence").at("template_identity"), 1311768467463790320ull);
    EXPECT_EQ(json.at("evidence").at("board_width"), 800);
    EXPECT_EQ(json.at("evidence").at("localization_score"), 0.97);
    EXPECT_EQ(json.at("evidence").at("board_hash").size(), 4u);
    EXPECT_TRUE(json.at("evidence").at("geometry_checked"));
    EXPECT_TRUE(json.at("evidence").at("geometry_anomaly"));
    EXPECT_EQ(json.at("evidence").at("geometry_drift_x"), 12.0);
    EXPECT_EQ(json.at("evidence").at("geometry_step_drift_x"), 13.0);
    EXPECT_EQ(json.at("evidence").at("geometry_decision"), "jump_detected");
    EXPECT_EQ(json.at("evidence").at("score_margin"), 12.5);
    EXPECT_EQ(json.at("evidence").at("yellow_endpoint_threshold"), 25.0);
    EXPECT_EQ(json.at("evidence").at("yellow_pair_threshold"), 70.0);
    ASSERT_EQ(json.at("evidence").at("yellow_measurements").size(), 1u);
    EXPECT_EQ(json.at("evidence").at("yellow_measurements").at(0).at("square"), "d4");
    EXPECT_EQ(json.at("evidence").at("yellow_measurements").at(0).at("corner_scores").at(2), 30.0);
    EXPECT_EQ(json.at("evidence").at("yellow_measurements").at(0).at("corner_bgr").at(0).at(1), 150.0);
    EXPECT_EQ(json.at("evidence").at("yellow_measurements").at(0).at("corner_edge_density").at(3), 0.11);
    EXPECT_TRUE(json.at("evidence").at("yellow_temporal_checked"));
    EXPECT_EQ(json.at("evidence").at("yellow_temporal_sample_count"), 3);
    EXPECT_EQ(json.at("evidence").at("yellow_temporal_pair_pass_count"), 2);
    EXPECT_EQ(json.at("evidence").at("yellow_temporal_max_pair"), 90.0);
    EXPECT_EQ(json.at("evidence").at("yellow_assessment").at("state"), "passed");
    EXPECT_EQ(json.at("evidence").at("yellow_assessment").at("confidence"), -1.0);
    EXPECT_EQ(json.at("evidence").at("yellow_assessment").at("measurements").at("pair_score"), 90.0);
    EXPECT_EQ(json.at("evidence").at("yellow_assessment").at("uncertainty_reason"), "uncalibrated_detector_confidence");
    ASSERT_EQ(json.at("evidence").at("changed_squares").size(), 1u);
    EXPECT_EQ(json.at("evidence").at("changed_squares").at(0).at("square"), "e4");
    ASSERT_EQ(json.at("evidence").at("yellow_candidates").size(), 1u);
    EXPECT_EQ(json.at("evidence").at("yellow_candidates").at(0).at("square"), "d4");
    EXPECT_EQ(json.at("metadata"), "reason=hover");
    EXPECT_EQ(json.at("evidence").at("changed_square_count"), 0);
    EXPECT_EQ(json.at("evidence").at("score_from_square_diff"), 42.0);
    EXPECT_EQ(json.at("evidence").at("score_adjustment"), -10.0);
    EXPECT_EQ(json.at("evidence").at("score_threshold_decision"), "passed");
    EXPECT_EQ(json.at("evidence").at("yellow_from"), 0.0);
    EXPECT_FALSE(json.at("evidence").at("hover_detected"));
    ASSERT_EQ(json.at("evidence").at("hover_measurements").size(), 1u);
    EXPECT_EQ(json.at("evidence").at("hover_measurements").at(0).at("square"), "e2");
    EXPECT_EQ(json.at("evidence").at("hover_measurements").at(0).at("visible_edges"), 3);
    EXPECT_TRUE(json.at("evidence").at("hover_measurements").at(0).at("detected"));
    EXPECT_EQ(json.at("evidence").at("clock_top_width"), 120);
    EXPECT_EQ(json.at("evidence").at("clock_bottom_height"), 24);
    EXPECT_EQ(json.at("evidence").at("clock_bright_ratio_delta"), 0.27);
    EXPECT_EQ(json.at("evidence").at("yellow_decision"), "passed");
    EXPECT_EQ(json.at("evidence").at("clock_decision"), "ocr_plausible");
    EXPECT_EQ(json.at("evidence").at("settle_decision"), "accepted_same_move");
    EXPECT_EQ(json.at("evidence").at("rejection_reason"), "hover_box");
    ASSERT_EQ(json.at("evidence").at("legal_candidates").size(), 2u);
    EXPECT_EQ(json.at("evidence").at("legal_candidates").at(0).at("move"), "e2e4");
    EXPECT_EQ(json.at("evidence").at("legal_candidates").at(0).at("rank"), 1);
}

TEST(VideoChunkMapperTest, ReplaysObservationArtifactsWithoutOpeningVideo) {
    const auto root = std::filesystem::current_path() / "build_tests" / "tmp" /
                      "cta_mapper_replay_test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);

    const auto board_path = root / "board.png";
    const auto trace_path = root / "observations.jsonl";
    cv::Mat board(80, 80, CV_8UC3, cv::Scalar(40, 80, 120));
    ASSERT_TRUE(cv::imwrite(board_path.string(), board));

    nlohmann::json observation = {
        {"schema_version", 1},
        {"observation_id", 42},
        {"timestamp", 1.25},
        {"mapper", {
            {"chunk", 0},
            {"source_frame_index", 77},
            {"emission_reason", "settled_tail"},
        }},
        {"images", {{"board", board_path.filename().string()}}},
        {"board", {{"hash", std::vector<double>(64, 12.0)}}},
    };
    {
        std::ofstream output(trace_path);
        ASSERT_TRUE(output.is_open());
        output << observation.dump() << '\n';
    }

    cta::BoardGeometry geometry;
    geometry.bx = 0;
    geometry.by = 0;
    geometry.bw = 80;
    geometry.bh = 80;
    geometry.sq_w = 10.0;
    geometry.sq_h = 10.0;
    std::atomic<bool> cancel{false};
    cta::VideoChunkMapper mapper(
        (root / "source-video-does-not-exist.mp4").string(), 5.0, 30.0, 1,
        geometry, 1, 1, 0, false, 1, 1, 80, 80,
        false, 5.0, std::string(), -1.0, -1.0, trace_path.string());
    mapper.start(&cancel);

    std::vector<cta::CandidateFrame> candidates;
    ASSERT_TRUE(mapper.get_chunk_results(0, candidates, &cancel));
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].observation_id, 42u);
    EXPECT_DOUBLE_EQ(candidates[0].t, 1.25);
    EXPECT_EQ(candidates[0].source_frame_index, 77u);
    EXPECT_EQ(candidates[0].emission_reason, "settled_tail");
    EXPECT_EQ(candidates[0].board_bgr.size(), cv::Size(80, 80));
    ASSERT_EQ(candidates[0].board_hash.size(), 64u);
    EXPECT_DOUBLE_EQ(candidates[0].board_hash.front(), 12.0);

    std::filesystem::remove_all(root, cleanup_error);
}

TEST(ChessVideoExtractorTest, ReplaysReducerFromObservationsWithoutOpeningVideo) {
    const auto root = std::filesystem::current_path() / "build_tests" / "tmp" /
                      "cta_extractor_replay_test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);

    const auto board_path = root / "board.png";
    const auto trace_path = root / "observations.jsonl";
    cv::Mat board(80, 80, CV_8UC3, cv::Scalar(40, 80, 120));
    ASSERT_TRUE(cv::imwrite(board_path.string(), board));

    const std::string initial_fen =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    nlohmann::json observation = {
        {"schema_version", 1},
        {"observation_id", 1},
        {"timestamp", 0.0},
        {"mapper", {{"chunk", 0}, {"source_frame_index", 1},
                     {"emission_reason", "initial_frame"}}},
        {"images", {{"board", board_path.filename().string()}}},
        {"board", {{"x", 0}, {"y", 0}, {"width", 80}, {"height", 80},
                    {"square_width", 10.0}, {"square_height", 10.0},
                    {"hash", std::vector<double>(64, 80.0)}}},
        {"events", {{{"sequence", 1}, {"event", "QUIET"},
                      {"active_ply", 0}, {"fen", initial_fen}}}},
    };
    {
        std::ofstream output(trace_path);
        ASSERT_TRUE(output.is_open());
        output << observation.dump() << '\n';
    }

    const char* previous_replay_path = std::getenv("CTA_REPLAY_OBSERVATIONS");
    const std::string previous_value = previous_replay_path ? previous_replay_path : "";
#ifdef _WIN32
    _putenv_s("CTA_REPLAY_OBSERVATIONS", trace_path.string().c_str());
#else
    setenv("CTA_REPLAY_OBSERVATIONS", trace_path.string().c_str(), 1);
#endif

    cta::GameData first_data;
    cta::GameData second_data;
    try {
        cta::ChessVideoExtractor first_extractor(board_path.string());
        first_data = first_extractor.extract_moves_from_video(
            (root / "source-video-does-not-exist.mp4").string(), "replay-test-first");
        cta::ChessVideoExtractor second_extractor(board_path.string());
        second_data = second_extractor.extract_moves_from_video(
            (root / "source-video-does-not-exist.mp4").string(), "replay-test");
    } catch (...) {
#ifdef _WIN32
        _putenv_s("CTA_REPLAY_OBSERVATIONS", previous_replay_path ? previous_value.c_str() : "");
#else
        if (previous_replay_path) setenv("CTA_REPLAY_OBSERVATIONS", previous_value.c_str(), 1);
        else unsetenv("CTA_REPLAY_OBSERVATIONS");
#endif
        std::filesystem::remove_all(root, cleanup_error);
        throw;
    }
#ifdef _WIN32
    _putenv_s("CTA_REPLAY_OBSERVATIONS", previous_replay_path ? previous_value.c_str() : "");
#else
    if (previous_replay_path) setenv("CTA_REPLAY_OBSERVATIONS", previous_value.c_str(), 1);
    else unsetenv("CTA_REPLAY_OBSERVATIONS");
#endif

    EXPECT_TRUE(first_data.moves.empty());
    ASSERT_FALSE(first_data.fens.empty());
    EXPECT_EQ(first_data.fens.front(), initial_fen);
    expect_game_data_equal(first_data, second_data);
    std::filesystem::remove_all(root, cleanup_error);
}

// ─── TEST CONTROL PANEL ─────────────────────────────────────────────────────
// Set to 1 to enable, 0 to disable. Comment/uncomment to toggle.
// Every test MUST have a toggle here — no exceptions.
//
// Unit tests (detector accuracy on sample images):
#define TEST_LOCATE_BOARD         1
#define TEST_DRAW_GRID            1
#define TEST_YELLOW_SQUARES       1
#define TEST_PIECE_COUNTS         1
#define TEST_RED_SQUARES          1
#define TEST_YELLOW_ARROWS        1
#define TEST_MISALIGNED_PIECE     1
#define TEST_GAME_CLOCKS          1
#define TEST_MEMORY_LIMIT         1
#define TEST_CACHE_CORRECTNESS    1
//
// Integration tests (full video pipeline with ground-truth PGN):
#define TEST_7_PLIES_EXTRACTION   1
#define TEST_MEDIUM_GAME_REVERT   1
#define TEST_FULL_GAME_1_EXTRACTION 1
#define TEST_INTEGRATION_CLOCK_TIMES 1
//
// Smoke tests (constructor/validation):
#define TEST_CONSTRUCTOR_THROWS   0
// ─────────────────────────────────────────────────────────────────────────────

namespace cta {

TEST(ExtractionInternalTest, FindsMostRecentBoundedInverseMove) {
    const std::vector<std::string> moves = {
        "a2a4", "h7h5", "a4a5", "g7g5", "b2b3"
    };

    const std::optional<size_t> inverse =
        extractor_detail::find_recent_inverse_move_index(moves, "a5", "a4");

    ASSERT_TRUE(inverse.has_value());
    EXPECT_EQ(*inverse, 2U);
    EXPECT_TRUE(extractor_detail::is_inverse_of_recent_move(moves, "a5", "a4"));
    EXPECT_FALSE(extractor_detail::find_recent_inverse_move_index(moves, "h5", "h4").has_value());
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string format_clock_for_test(const std::string& raw) {
    if (raw.empty()) return "";

    std::string cleaned;
    bool last_was_colon = false;
    for (char c : raw) {
        if (std::isdigit(c) || c == '.') {
            cleaned += c;
            last_was_colon = false;
        } else if (c == ':') {
            if (!last_was_colon) {
                cleaned += c;
                last_was_colon = true;
            }
        }
    }
    if (!cleaned.empty() && cleaned.front() == ':') cleaned = cleaned.substr(1);
    if (!cleaned.empty() && cleaned.back() == ':') cleaned.pop_back();

    std::vector<std::string> parts;
    std::stringstream ss(cleaned);
    std::string item;
    while (std::getline(ss, item, ':')) {
        parts.push_back(item);
    }
    if (parts.size() > 3 || parts.empty()) return "";

    int h = 0, m = 0;
    double s = 0.0;
    try {
        if (parts.size() == 3) { h = std::stoi(parts[0]); m = std::stoi(parts[1]); s = std::stod(parts[2]); } 
        else if (parts.size() == 2) { m = std::stoi(parts[0]); s = std::stod(parts[1]); } 
        else { s = std::stod(parts[0]); }

        if (s >= 60.0) return "";
        if (m > 59) { h += m / 60; m = m % 60; }
    } catch (...) { return ""; }

    std::ostringstream out;
    out << h << ":" << std::setfill('0') << std::setw(2) << m << ":";
    if (s < 10.0) out << "0";
    
    if (std::floor(s) == s) {
        out << static_cast<int>(s);
    } else {
        out << std::fixed << std::setprecision(1) << s;
    }
    return out.str();
}

// Helper to find the assets directory from various common CWDs (root, build, build/Release)
static std::string find_assets_dir() {
    std::vector<std::filesystem::path> paths_to_check = {
        "assets",
        "../assets",
        "../../assets",
        "../../../assets" // Legacy, just in case
    };
    for (const auto& p : paths_to_check) {
        if (std::filesystem::exists(p) && std::filesystem::is_directory(p)) {
            return p.string();
        }
    }
    // Special case for Visual Studio CWD which can be $(SolutionDir)
    if (std::filesystem::exists("ChessTubeAnalyzer/assets")) {
        return "ChessTubeAnalyzer/assets";
    }
    return ""; // Not found
}

static std::string normalize_san_token(std::string san) {
    san.erase(std::remove(san.begin(), san.end(), '+'), san.end());
    san.erase(std::remove(san.begin(), san.end(), '#'), san.end());
    san.erase(std::remove(san.begin(), san.end(), '!'), san.end());
    san.erase(std::remove(san.begin(), san.end(), '?'), san.end());
    san.erase(std::remove(san.begin(), san.end(), '='), san.end());
    san.erase(std::remove(san.begin(), san.end(), 'x'), san.end());

    auto ep_pos = san.find("e.p.");
    if (ep_pos != std::string::npos) san.erase(ep_pos, 4);

    if (san == "0-0") return "O-O";
    if (san == "0-0-0") return "O-O-O";
    return san;
}

static std::array<char, 64> expand_fen_board_for_test(const std::string& fen) {
    std::array<char, 64> board;
    board.fill(' ');
    int sq = 56;
    for (char c : fen) {
        if (c == ' ') break;
        if (c == '/') sq -= 16;
        else if (c >= '1' && c <= '8') sq += (c - '0');
        else board[sq++] = c;
    }
    return board;
}

static std::string build_san_for_test(const libchess::Position& pos, const libchess::Move& move, const std::string& uci) {
    auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
    auto to_sq = static_cast<int>(static_cast<unsigned int>(move.to()));
    std::array<char, 64> board = expand_fen_board_for_test(pos.get_fen());
    char piece = board[from_sq];
    char target_piece = board[to_sq];
    bool is_pawn = (piece == 'P' || piece == 'p');
    bool is_capture = (target_piece != ' ') || (is_pawn && (from_sq % 8) != (to_sq % 8) && target_piece == ' ');

    if (move.type() == libchess::MoveType::ksc) return "O-O";
    if (move.type() == libchess::MoveType::qsc) return "O-O-O";

    std::string san;
    if (!is_pawn) {
        san += static_cast<char>(std::toupper(piece));
        bool file_conflict = false;
        bool rank_conflict = false;
        bool need_disambiguation = false;
        for (const auto& alt_move : pos.legal_moves()) {
            auto alt_from = static_cast<int>(static_cast<unsigned int>(alt_move.from()));
            auto alt_to = static_cast<int>(static_cast<unsigned int>(alt_move.to()));
            if (alt_from != from_sq && alt_to == to_sq && board[alt_from] == piece) {
                need_disambiguation = true;
                if (alt_from % 8 == from_sq % 8) file_conflict = true;
                if (alt_from / 8 == from_sq / 8) rank_conflict = true;
            }
        }
        if (need_disambiguation) {
            if (!file_conflict) san += static_cast<char>('a' + (from_sq % 8));
            else if (!rank_conflict) san += static_cast<char>('1' + (from_sq / 8));
            else {
                san += static_cast<char>('a' + (from_sq % 8));
                san += static_cast<char>('1' + (from_sq / 8));
            }
        }
    } else if (is_capture) {
        san += static_cast<char>('a' + (from_sq % 8));
    }

    if (is_capture) san += "x";
    san += static_cast<char>('a' + (to_sq % 8));
    san += static_cast<char>('1' + (to_sq / 8));
    if (uci.length() >= 5) {
        san += "=";
        san += static_cast<char>(std::toupper(uci[4]));
    }

    libchess::Position next_pos = pos;
    next_pos.makemove(move);
    if (next_pos.is_checkmate()) san += "#";
    else if (next_pos.in_check()) san += "+";
    return san;
}

struct ExpectedGameData {
    std::vector<std::string> main_line;
    std::vector<std::string> variations;
    std::vector<std::string> all_moves;
    std::vector<std::pair<size_t, std::vector<std::string>>> variation_sequences;
};

static std::string expected_fen_after_ply(const std::vector<std::string>& moves,
                                          size_t ply_count) {
    libchess::Position position(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    const size_t count = std::min(ply_count, moves.size());
    for (size_t ply = 0; ply < count; ++ply) {
        try {
            position.makemove(position.parse_move(moves[ply]));
        } catch (...) {
            return {};
        }
    }
    return position.get_fen();
}

static size_t first_main_line_mismatch(const std::vector<std::string>& expected,
                                       const std::vector<std::string>& extracted) {
    size_t mismatch = 0;
    while (mismatch < expected.size() && mismatch < extracted.size() &&
           expected[mismatch] == extracted[mismatch]) {
        ++mismatch;
    }
    return mismatch;
}

static void write_first_divergence_report(
    const std::string& test_name,
    const std::string& video_path,
    const std::vector<std::string>& expected_moves,
    const GameData& data,
    size_t mismatch,
    bool main_line_passed,
    bool complete_output_passed,
    const std::string& failure_scope) {
    const char* report_path = std::getenv("CTA_FAILURE_REPORT_FILE");
    if (report_path == nullptr || *report_path == '\0' ||
        (main_line_passed && complete_output_passed)) {
        return;
    }

    const bool expected_move_exists = mismatch < expected_moves.size();
    const bool extracted_move_exists = mismatch < data.moves.size();
    const double anchor_timestamp = extracted_move_exists && mismatch < data.timestamps.size()
        ? data.timestamps[mismatch]
        : (!data.timestamps.empty() ? data.timestamps.back() : 0.0);

    std::string failure_kind = failure_scope;
    if (failure_kind.empty()) {
        if (!expected_move_exists) failure_kind = "unexpected_extra_move";
        else if (!extracted_move_exists) failure_kind = "missing_expected_move";
        else failure_kind = "move_mismatch";
    }

    const std::string expected_before_fen = expected_fen_after_ply(expected_moves, mismatch);
    const std::string expected_after_fen = expected_move_exists
        ? expected_fen_after_ply(expected_moves, mismatch + 1)
        : expected_before_fen;
    const std::string extracted_before_fen = mismatch < data.fens.size()
        ? data.fens[mismatch] : std::string{};
    const std::string extracted_after_fen = mismatch + 1 < data.fens.size()
        ? data.fens[mismatch + 1] : std::string{};

    nlohmann::json report = {
        {"schema_version", 1},
        {"test", test_name},
        {"video", video_path},
        {"failure_kind", failure_kind},
        {"failure_scope", failure_scope},
        {"first_mismatch_ply", mismatch + 1},
        {"last_matching_ply", mismatch},
        {"expected_move", expected_move_exists ? expected_moves[mismatch] : ""},
        {"extracted_move", extracted_move_exists ? data.moves[mismatch] : ""},
        {"expected_move_count", expected_moves.size()},
        {"extracted_move_count", data.moves.size()},
        {"anchor_timestamp", anchor_timestamp},
        {"expected_before_fen", expected_before_fen},
        {"expected_after_fen", expected_after_fen},
        {"extracted_before_fen", extracted_before_fen},
        {"extracted_after_fen", extracted_after_fen},
        {"main_line_passed", main_line_passed},
        {"complete_output_passed", complete_output_passed},
    };

    std::ofstream output(report_path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        std::cerr << "Could not write first-divergence report: " << report_path << "\n";
        return;
    }
    output << report.dump(2) << '\n';
}

struct ExpectedClockData {
    std::vector<std::string> main_line;
    std::vector<std::string> variations;
};

static ExpectedClockData load_expected_clocks_from_pgn(const std::string& pgn_path) {
    std::ifstream ifs(pgn_path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Could not open PGN: " + pgn_path);
    }

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ExpectedClockData clocks;
    bool in_header = false;
    int brace_depth = 0;
    int variation_depth = 0;

    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];

        if (c == '[' && brace_depth == 0) {
            in_header = true;
            continue;
        }
        if (in_header) {
            if (c == ']') in_header = false;
            continue;
        }

        if (c == '{') {
            ++brace_depth;
            continue;
        }
        if (c == '}' && brace_depth > 0) {
            --brace_depth;
            continue;
        }

        if (brace_depth == 0) {
            if (c == '(') ++variation_depth;
            else if (c == ')' && variation_depth > 0) --variation_depth;
            continue;
        }

        if (variation_depth == 0 && content.compare(i, 6, "[%clk ") == 0) {
            size_t start = i + 6;
            size_t end = content.find(']', start);
            if (end != std::string::npos) {
                clocks.main_line.push_back(format_clock_for_test(content.substr(start, end - start)));
                i = end;
            }
        } else if (variation_depth > 0 && content.compare(i, 6, "[%clk ") == 0) {
            size_t start = i + 6;
            size_t end = content.find(']', start);
            if (end != std::string::npos) {
                clocks.variations.push_back(format_clock_for_test(content.substr(start, end - start)));
                i = end;
            }
        }
    }

    return clocks;
}

static std::string clock_for_ply(const ClockInfo& clock, size_t ply_index) {
    std::string raw_clock = (ply_index % 2 == 0) ? clock.white_time : clock.black_time;
    std::string formatted = format_clock_for_test(raw_clock);
    return formatted.empty() ? "0:00:00" : formatted;
}

static ExpectedClockData extract_clocks_from_game_data(const GameData& data) {
    ExpectedClockData clocks;
    clocks.main_line.reserve(data.moves.size());

    for (size_t i = 0; i < data.moves.size(); ++i) {
        size_t clockIdx = i + 1;
        const auto* clk_ptr = (clockIdx < data.clocks.size())
            ? &data.clocks[clockIdx]
            : (i < data.clocks.size()) ? &data.clocks[i] : nullptr;
        clocks.main_line.push_back(clk_ptr ? clock_for_ply(*clk_ptr, i) : "0:00:00");
    }

    for (const auto& item : data.variations) {
        size_t branch_ply = item.first;
        for (const auto& var : item.second) {
            for (size_t j = 0; j < var.moves.size(); ++j) {
                if (j < var.clocks.size()) {
                    clocks.variations.push_back(clock_for_ply(var.clocks[j], branch_ply + j));
                }
            }
        }
    }

    return clocks;
}

static void print_multiset_delta(const std::multiset<std::string>& extracted,
                                 const std::multiset<std::string>& expected,
                                 const std::string& label);

static bool verify_clocks(const std::string& pgn_path, const GameData& data) {
    ExpectedClockData expected_clocks = load_expected_clocks_from_pgn(pgn_path);
    ExpectedClockData extracted_clocks = extract_clocks_from_game_data(data);

    std::cout << "  Loaded expected clocks from PGN.\n";
    std::cout << "  Expected Main-Line Clocks (" << expected_clocks.main_line.size() << "): ";
    for (const auto& c : expected_clocks.main_line) std::cout << c << " ";
    std::cout << "\n";

    std::cout << "  Extracted Main-Line Clocks (" << extracted_clocks.main_line.size() << "): ";
    for (const auto& c : extracted_clocks.main_line) std::cout << c << " ";
    std::cout << "\n";

    std::cout << "  Expected Variation Clocks (" << expected_clocks.variations.size() << "): ";
    for (const auto& c : expected_clocks.variations) std::cout << c << " ";
    std::cout << "\n";

    std::cout << "  Extracted Variation Clocks (" << extracted_clocks.variations.size() << "): ";
    for (const auto& c : extracted_clocks.variations) std::cout << c << " ";
    std::cout << "\n";

    for (size_t i = 0; i < std::min(expected_clocks.main_line.size(), extracted_clocks.main_line.size()); ++i) {
        if (expected_clocks.main_line[i] != extracted_clocks.main_line[i]) {
            std::cout << "  First main-line clock mismatch at ply " << (i + 1)
                      << ": expected " << expected_clocks.main_line[i]
                      << ", extracted " << extracted_clocks.main_line[i] << "\n";
            break;
        }
    }

    std::multiset<std::string> expected_variation_clocks(expected_clocks.variations.begin(), expected_clocks.variations.end());
    std::multiset<std::string> extracted_variation_clocks(extracted_clocks.variations.begin(), extracted_clocks.variations.end());
    print_multiset_delta(extracted_variation_clocks, expected_variation_clocks, "variation clocks");

    EXPECT_EQ(extracted_clocks.main_line, expected_clocks.main_line)
        << "Extracted " << extracted_clocks.main_line.size() << " main-line clocks, expected "
        << expected_clocks.main_line.size();
    EXPECT_EQ(extracted_variation_clocks, expected_variation_clocks)
        << "Extracted " << extracted_clocks.variations.size() << " variation clocks, expected "
        << expected_clocks.variations.size();

    return extracted_clocks.main_line == expected_clocks.main_line
        && extracted_variation_clocks == expected_variation_clocks;
}

struct PgnState {
    libchess::Position pos;
    std::vector<libchess::Position> history;
    PgnState() : pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") {}
};

static ExpectedGameData load_expected_uci_moves_from_pgn(const std::string& pgn_path) {
    std::ifstream ifs(pgn_path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Could not open PGN: " + pgn_path);
    }

    std::stringstream buffer;
    buffer << ifs.rdbuf();

    ExpectedGameData result;
    
    std::string pgn = buffer.str();
    std::string cleaned;
    bool in_header = false;
    int brace_depth = 0;
    for (size_t i = 0; i < pgn.size(); ++i) {
        char c = pgn[i];
        if (c == '[' && brace_depth == 0) { in_header = true; continue; }
        if (in_header && c == ']') { in_header = false; continue; }
        if (in_header) continue;

        if (c == '{') { ++brace_depth; continue; }
        if (c == '}' && brace_depth > 0) { --brace_depth; continue; }
        if (brace_depth > 0) continue;

        if (c == '(' || c == ')') {
            cleaned += ' ';
            cleaned += c;
            cleaned += ' ';
        } else {
            cleaned += c;
        }
    }

    std::istringstream iss(cleaned);
    std::string token;
    
    PgnState current_state;
    std::vector<PgnState> state_stack;
    std::vector<size_t> active_variation_sequences;
    auto try_parse_move = [](const PgnState& state, const std::string& token, libchess::Move& move_out) {
        const std::string wanted_san = normalize_san_token(token);
        if (wanted_san.empty()) return false;

        try {
            move_out = state.pos.parse_move(token);
            return true;
        } catch (...) {
            try {
                move_out = state.pos.parse_move(wanted_san);
                return true;
            } catch (...) {
                for (const auto& legal_move : state.pos.legal_moves()) {
                    std::string uci = static_cast<std::string>(legal_move);
                    if (uci == "e1h1") uci = "e1g1"; else if (uci == "e1a1") uci = "e1c1";
                    else if (uci == "e8h8") uci = "e8g8"; else if (uci == "e8a8") uci = "e8c8";
                    if (normalize_san_token(build_san_for_test(state.pos, legal_move, uci)) == wanted_san) {
                        move_out = legal_move;
                        return true;
                    }
                }
            }
        }

        return false;
    };
    
    while (iss >> token) {
        if (token == "*" || token == "1-0" || token == "0-1" || token == "1/2-1/2") continue;
        if (token.rfind("$", 0) == 0) continue;

        if (token == "(") {
            state_stack.push_back(current_state);
            result.variation_sequences.emplace_back(current_state.history.size(), std::vector<std::string>{});
            active_variation_sequences.push_back(result.variation_sequences.size() - 1);
            if (!current_state.history.empty()) {
                current_state.pos = current_state.history.back();
            }
            continue;
        }
        if (token == ")") {
            if (!state_stack.empty()) {
                current_state = state_stack.back();
                state_stack.pop_back();
            }
            if (!active_variation_sequences.empty()) {
                active_variation_sequences.pop_back();
            }
            continue;
        }

        size_t first_dot = token.find('.');
        if (first_dot != std::string::npos) {
            size_t last_dot = first_dot;
            while (last_dot < token.size() && token[last_dot] == '.') last_dot++;
            token = token.substr(last_dot);
        }
        if (token.empty()) continue;

        bool is_all_digits = true;
        for (char c : token) { if (!std::isdigit(static_cast<unsigned char>(c))) { is_all_digits = false; break; } }
        if (is_all_digits) continue;

        bool matched = false;
        libchess::Move m;

        if (normalize_san_token(token).empty()) continue;

        matched = try_parse_move(current_state, token, m);
        if (!matched && !state_stack.empty()) {
            // Some hand-authored baselines use parentheses for a checked line
            // continuation rather than an alternative to the previous move.
            PgnState continuation_state = state_stack.back();
            if (try_parse_move(continuation_state, token, m)) {
                current_state = continuation_state;
                matched = true;
            }
        }

        if (matched) {
            std::string uci = static_cast<std::string>(m);
            if (uci == "e1h1") uci = "e1g1"; else if (uci == "e1a1") uci = "e1c1";
            else if (uci == "e8h8") uci = "e8g8"; else if (uci == "e8a8") uci = "e8c8";
            
            if (state_stack.empty()) {
                result.main_line.push_back(uci);
            } else {
                result.variations.push_back(uci);
            }
            result.all_moves.push_back(uci);
            if (!active_variation_sequences.empty()) {
                result.variation_sequences[active_variation_sequences.back()].second.push_back(uci);
            }
            
            current_state.history.push_back(current_state.pos);
            current_state.pos.makemove(m);
        } else {
            throw std::runtime_error("Could not convert PGN SAN token to UCI: " + token);
        }
    }

    return result;
}

static std::multiset<std::string> extract_all_moves_multiset(const GameData& data) {
    std::multiset<std::string> all(data.moves.begin(), data.moves.end());
    for (const auto& item : data.variations) {
        for (const auto& var : item.second) {
            for (const auto& m : var.moves) {
                all.insert(m);
            }
        }
    }
    return all;
}

static bool verify_game_data_invariants(const GameData& data) {
    bool valid = true;
    const auto fail = [&](const std::string& message) {
        valid = false;
        ++g_invariant_failure_count;
        write_invariant_diagnostic(message);
        ADD_FAILURE() << "GameData invariant: " << message;
    };

    if (data.fens.size() != data.moves.size() + 1) {
        fail("main-line FEN count " + std::to_string(data.fens.size()) +
             " does not equal move count + 1 (" + std::to_string(data.moves.size() + 1) + ")");
    }
    if (data.timestamps.size() != data.moves.size()) {
        fail("main-line timestamp count " + std::to_string(data.timestamps.size()) +
             " does not equal move count " + std::to_string(data.moves.size()));
    }
    if (data.clocks.size() != data.fens.size()) {
        fail("main-line clock count " + std::to_string(data.clocks.size()) +
             " does not equal FEN count " + std::to_string(data.fens.size()));
    }
    const auto verify_clock_provenance = [&](const ClockInfo& clock, const std::string& location) {
        if (clock.moved_time_observed && clock.moved_time_missing) {
            fail("clock at " + location + " is both observed and missing");
        }
        if (clock.moved_time_observed && clock.white_time.empty() && clock.black_time.empty()) {
            fail("clock at " + location + " is marked observed but contains no reading");
        }
    };
    for (size_t i = 0; i < data.clocks.size(); ++i) {
        verify_clock_provenance(data.clocks[i], "main ply " + std::to_string(i));
    }
    if (data.video_moves.size() != data.video_timestamps.size()) {
        fail("video move and timestamp histories have different lengths");
    }
    if (data.video_fens.size() != data.video_moves.size() + 1) {
        fail("video FEN history " + std::to_string(data.video_fens.size()) +
             " does not equal video move count + 1 (" +
             std::to_string(data.video_moves.size() + 1) + ")");
    }

    if (!data.fens.empty()) {
        try {
            libchess::Position position(data.fens.front());
            for (size_t ply = 0; ply < data.moves.size(); ++ply) {
                if (ply >= data.fens.size() - 1) break;
                if (data.fens[ply] != position.get_fen()) {
                    fail("FEN before ply " + std::to_string(ply + 1) + " differs from reducer position");
                }
                const libchess::Move move = position.parse_move(data.moves[ply]);
                position.makemove(move);
                if (data.fens[ply + 1] != position.get_fen()) {
                    fail("FEN after ply " + std::to_string(ply + 1) +
                         " does not match the accepted move " + data.moves[ply]);
                }
            }
        } catch (const std::exception& error) {
            fail(std::string("main-line move/FEN replay failed: ") + error.what());
        }
    }

    for (size_t i = 1; i < data.timestamps.size(); ++i) {
        if (data.timestamps[i] < data.timestamps[i - 1]) {
            fail("main-line timestamps are not monotonic at ply " + std::to_string(i + 1));
            break;
        }
    }
    for (size_t i = 1; i < data.video_timestamps.size(); ++i) {
        if (data.video_timestamps[i] < data.video_timestamps[i - 1]) {
            fail("video timestamps are not monotonic at observation " + std::to_string(i));
            break;
        }
    }

    for (const auto& [parent_ply, variations] : data.variations) {
        if (parent_ply >= data.fens.size()) {
            fail("variation parent ply " + std::to_string(parent_ply) + " is outside the main line");
            continue;
        }
        for (const auto& variation : variations) {
            if (variation.moves.empty()) continue;
            const bool has_terminal_fen = variation.fens.size() == variation.moves.size() + 1;
            if (variation.fens.size() != variation.moves.size() && !has_terminal_fen) {
                fail("variation at parent ply " + std::to_string(parent_ply) +
                     " has " + std::to_string(variation.fens.size()) +
                     " FENs for " + std::to_string(variation.moves.size()) +
                     " moves; expected before-state FENs or before/after FENs");
            }
            if (!variation.fens.empty() && variation.fens.front() != data.fens[parent_ply]) {
                fail("variation at parent ply " + std::to_string(parent_ply) +
                     " does not start from its parent FEN");
            }
            if (variation.timestamps.size() != variation.moves.size()) {
                fail("variation at parent ply " + std::to_string(parent_ply) +
                     " has misaligned move and timestamp histories");
            }
            for (size_t i = 0; i < variation.clocks.size(); ++i) {
                verify_clock_provenance(
                    variation.clocks[i],
                    "variation parent ply " + std::to_string(parent_ply) +
                    ", index " + std::to_string(i));
            }
            for (size_t i = 1; i < variation.timestamps.size(); ++i) {
                if (variation.timestamps[i] < variation.timestamps[i - 1]) {
                    fail("variation timestamps are not monotonic at parent ply " +
                         std::to_string(parent_ply));
                    break;
                }
            }
            try {
                libchess::Position position(data.fens[parent_ply]);
                for (size_t i = 0; i < variation.moves.size(); ++i) {
                    if (i < variation.fens.size() && variation.fens[i] != position.get_fen()) {
                        fail("variation at parent ply " + std::to_string(parent_ply) +
                             " has an invalid FEN before move " + std::to_string(i + 1));
                    }
                    const libchess::Move move = position.parse_move(variation.moves[i]);
                    position.makemove(move);
                    if (i + 1 < variation.fens.size() &&
                        variation.fens[i + 1] != position.get_fen()) {
                        fail("variation at parent ply " + std::to_string(parent_ply) +
                             " has an invalid FEN after move " + std::to_string(i + 1));
                    }
                }
            } catch (const std::exception& error) {
                fail("variation at parent ply " + std::to_string(parent_ply) +
                     " failed legal replay: " + error.what());
            }
        }
    }

    return valid;
}

static void print_multiset_delta(const std::multiset<std::string>& extracted,
                                 const std::multiset<std::string>& expected,
                                 const std::string& label) {
    std::vector<std::string> extra;
    std::vector<std::string> missing;
    std::set_difference(extracted.begin(), extracted.end(),
                        expected.begin(), expected.end(),
                        std::back_inserter(extra));
    std::set_difference(expected.begin(), expected.end(),
                        extracted.begin(), extracted.end(),
                        std::back_inserter(missing));
    if (extra.empty() && missing.empty()) {
        return;
    }

    std::cout << "  " << label << " extra (" << extra.size() << "): ";
    for (const auto& item : extra) std::cout << item << " ";
    std::cout << "\n";
    std::cout << "  " << label << " missing (" << missing.size() << "): ";
    for (const auto& item : missing) std::cout << item << " ";
    std::cout << "\n";
}

static std::vector<std::string> list_files(const std::string& dir,
                                            const std::vector<std::string>& exts) {
    std::vector<std::string> result;
    if (!std::filesystem::exists(dir)) return result;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        for (const auto& e : exts) {
            if (ext == e) { result.push_back(entry.path().string()); break; }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

static std::string stem(const std::string& path) {
    return std::filesystem::path(path).stem().string();
}

// Get video duration in seconds using OpenCV
static double get_video_duration(const std::string& video_path) {
    cv::VideoCapture cap(video_path);
    if (!cap.isOpened()) return 0.0;
    double fps = cap.get(cv::CAP_PROP_FPS);
    double frames = cap.get(cv::CAP_PROP_FRAME_COUNT);
    cap.release();
    return (fps > 0) ? frames / fps : 0.0;
}

// Print the summary table after all tests
static void print_test_summary() {
    if (g_test_results.empty()) return;

    static const std::string sep(120, '-');
    static const std::string border(120, '=');

    std::cout << "\n";
    std::cout << border << "\n";
    std::cout << "  INTEGRATION TEST SUMMARY\n";
    std::cout << border << "\n";
    std::cout << std::left;
    std::cout << "  " << std::setw(28) << "Test"
              << std::setw(14) << "Video"
              << std::setw(10) << "Plies"
              << std::setw(10) << "Result"
              << std::setw(12) << "Extracted"
              << std::setw(10) << "Reverts"
              << std::setw(12) << "Time"
              << std::setw(12) << "Accuracy"
              << "\n";
    std::cout << "  " << sep << "\n";

    int total_passed = 0, total_plies = 0;
    double total_video = 0.0, total_proc = 0.0;

    auto fmt_time = [](double sec) -> std::string {
        if (sec < 60.0) return std::to_string(static_cast<int>(sec)) + "s";
        int m = static_cast<int>(sec) / 60;
        int s = static_cast<int>(sec) % 60;
        return std::to_string(m) + "m" + std::to_string(s) + "s";
    };

    for (const auto& r : g_test_results) {
        std::string result_str = r.passed ? "PASS" : "FAIL";
        std::string accuracy;
        if (r.plies_expected > 0) {
            int matched = std::min(r.plies_extracted, r.plies_expected);
            accuracy = std::to_string(matched) + "/" + std::to_string(r.plies_expected);
        } else {
            accuracy = "N/A";
        }

        std::cout << "  " << std::setw(28) << r.name
                  << std::setw(14) << fmt_time(r.video_duration_sec)
                  << std::setw(10) << r.plies_expected
                  << std::setw(10) << result_str
                  << std::setw(12) << std::to_string(r.plies_extracted)
                  << std::setw(10) << r.reverts_detected
                  << std::setw(12) << fmt_time(r.elapsed_sec)
                  << std::setw(12) << accuracy
                  << "\n";

        total_passed += r.passed ? 1 : 0;
        total_plies += r.plies_extracted;
        total_video += r.video_duration_sec;
        total_proc += r.elapsed_sec;
    }

    std::cout << "  " << sep << "\n";
    std::cout << "  " << total_passed << "/" << g_test_results.size() << " passed"
              << " | " << total_plies << " plies"
              << " | Video: " << fmt_time(total_video)
              << " | Processing: " << fmt_time(total_proc);
    if (total_video > 0 && total_proc > 0) {
        std::cout << " (" << std::fixed << std::setprecision(1) << (total_video / total_proc) << "x real-time)";
    }
    std::cout << "\n";
    std::cout << border << "\n";
    std::cout << "\n";
}

// ── Shared fixture ────────────────────────────────────────────────────────────

class DetectorsTest : public ::testing::Test {
protected:
    void SetUp() override {
        assets_dir_ = find_assets_dir();
        if (assets_dir_.empty()) {
            GTEST_SKIP() << "Could not find 'assets' directory. Tests skipped.";
        }

        board_path_ = (std::filesystem::path(assets_dir_) / "board" / "board.png").string();
        red_board_path_ = (std::filesystem::path(assets_dir_) / "board" / "red_board.png").string();

        board_ = cv::imread(board_path_);
        if (board_.empty()) {
            GTEST_SKIP() << "Board template not found: " << board_path_;
        }
        geo_ = locate_board(board_, board_);
    }

    std::string assets_dir_;
    std::string board_path_;
    std::string red_board_path_;
    cv::Mat board_;
    BoardGeometry geo_;
};

// ─── BOARD LOCALIZER ─────────────────────────────────────────────────────────
#if TEST_LOCATE_BOARD

TEST_F(DetectorsTest, LocateBoardOnItself) {
    auto geo = locate_board(board_, board_);
    EXPECT_GT(geo.bw, 0);
    EXPECT_GT(geo.bh, 0);
    EXPECT_NEAR(geo.sq_w, static_cast<double>(geo.bw) / 8.0, 1.0);
    EXPECT_NEAR(geo.sq_h, static_cast<double>(geo.bh) / 8.0, 1.0);
    EXPECT_GT(geo.localization_score, -1.0);
    EXPECT_GT(geo.localization_scale, 0.0);
}

#endif // TEST_LOCATE_BOARD

#if TEST_DRAW_GRID

TEST_F(DetectorsTest, DrawBoardGrid) {
    cv::Mat test_img = cv::Mat(800, 800, CV_8UC3, cv::Scalar(128, 128, 128));
    BoardGeometry geo{50, 50, 700, 700, 87.5, 87.5};
    EXPECT_NO_THROW(draw_board_grid(test_img, geo, cv::Scalar(0, 255, 0), 2, true));
}

#endif // TEST_DRAW_GRID

// ─── YELLOW SQUARE EXTRACTION ────────────────────────────────────────────────
#if TEST_YELLOW_SQUARES

TEST_F(DetectorsTest, YellowSquares) {
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "sample_yellow_squares").string();
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    std::cout << "\nRunning unit tests on yellow square images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string expected_name = stem(img_path);
        BoardGeometry img_geo = locate_board(img, board_);
        std::string move_uci = extract_move_from_yellow_squares(img, board_, img_geo);

        std::string clean = expected_name;
        clean.erase(std::remove(clean.begin(), clean.end(), '+'), clean.end());
        clean.erase(std::remove(clean.begin(), clean.end(), '#'), clean.end());

        std::string expected_dest;
        if (clean == "O-O") {
            expected_dest = (move_uci.size() >= 4 && move_uci[3] == '1') ? "g1" : "g8";
        } else if (clean == "O-O-O") {
            expected_dest = (move_uci.size() >= 4 && move_uci[3] == '1') ? "c1" : "c8";
        } else {
            expected_dest = clean.substr(clean.size() - 2);
        }

        std::string extracted_dest = move_uci.substr(2, 2);
        bool pass = (extracted_dest == expected_dest);
        std::cout << "  " << (pass ? "PASS" : "FAIL") << ": " << stem(img_path)
                  << " -> " << move_uci
                  << " (dest=" << extracted_dest << ", expected=" << expected_dest << ")\n";
        EXPECT_EQ(extracted_dest, expected_dest) << "Failed on " << img_path;
    }
    std::cout << "PASS: Extracted valid moves from " << files.size() << " yellow square images.\n";
}

#endif // TEST_YELLOW_SQUARES

// ─── PIECE COUNTING ──────────────────────────────────────────────────────────
#if TEST_PIECE_COUNTS

TEST_F(DetectorsTest, PieceCounts) {
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "sample_piece_counts").string();
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    std::cout << "\nRunning unit tests on piece counting images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        int expected = std::stoi(stem(img_path));
        BoardGeometry img_geo = locate_board(img, board_);
        int actual = count_pieces_in_image(img, board_, img_geo);
        bool pass = (actual == expected);
        std::cout << "  " << (pass ? "PASS" : "FAIL") << ": " << stem(img_path)
                  << " -> counted " << actual << " (expected " << expected << ")\n";
        EXPECT_EQ(actual, expected) << "Failed on " << img_path;
    }
    std::cout << "PASS: Accurately counted pieces in all " << files.size() << " images.\n";
}

#endif // TEST_PIECE_COUNTS

// ─── RED SQUARES ─────────────────────────────────────────────────────────────
#if TEST_RED_SQUARES

TEST_F(DetectorsTest, RedSquares) {
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "sample_red_squares").string();
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    cv::Mat red_board = cv::imread(red_board_path_);

    std::cout << "\nRunning unit tests on red square images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string expected_str = stem(img_path);
        std::vector<std::string> expected;
        std::string token;
        for (char c : expected_str) {
            if (c == ',') {
                if (!token.empty()) { expected.push_back(token); token.clear(); }
            } else if (c != ' ') {
                token += c;
            }
        }
        if (!token.empty()) expected.push_back(token);
        std::sort(expected.begin(), expected.end());

        BoardGeometry img_geo = locate_board(img, board_);
        auto actual = find_red_squares(img, board_, red_board, img_geo);
        bool pass = (actual == expected);
        std::string actual_str, exp_str;
        for (const auto& s : actual) { if (!actual_str.empty()) actual_str += ","; actual_str += s; }
        for (const auto& s : expected) { if (!exp_str.empty()) exp_str += ","; exp_str += s; }
        std::cout << "  " << (pass ? "PASS" : "FAIL") << ": " << stem(img_path)
                  << " -> [" << actual_str << "]" << (pass ? "" : " (expected [" + exp_str + "])") << "\n";
        EXPECT_EQ(actual, expected) << "Failed on " << img_path;
    }
    std::cout << "PASS: Accurately detected red squares in all " << files.size() << " images.\n";
}

#endif // TEST_RED_SQUARES

// ─── YELLOW ARROWS ───────────────────────────────────────────────────────────
#if TEST_YELLOW_ARROWS

TEST_F(DetectorsTest, YellowArrows) {
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "sample_yellow_arrows").string();
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    std::cout << "\nRunning unit tests on yellow arrow images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string expected_str = stem(img_path);
        std::vector<std::string> expected;
        for (size_t i = 0; i < expected_str.size(); ) {
            if (i + 4 <= expected_str.size()) {
                expected.push_back(expected_str.substr(i, 4));
                i += 4;
                if (i < expected_str.size() && expected_str[i] == ',') ++i;
            } else break;
        }
        std::sort(expected.begin(), expected.end());

        auto to_endpoints = [](const std::vector<std::string>& arrows) {
            std::vector<std::string> eps;
            for (const auto& a : arrows) {
                if (a.size() >= 4) {
                    std::string e1 = a.substr(0, 2);
                    std::string e2 = a.substr(2, 4);
                    if (e1 < e2) eps.push_back(e1 + e2);
                    else eps.push_back(e2 + e1);
                }
            }
            std::sort(eps.begin(), eps.end());
            return eps;
        };

        BoardGeometry img_geo = locate_board(img, board_);
        auto actual = find_yellow_arrows(img, board_, img_geo);
        bool pass = (to_endpoints(actual) == to_endpoints(expected));
        std::string actual_str, exp_str;
        for (const auto& s : actual) { if (!actual_str.empty()) actual_str += ","; actual_str += s; }
        for (const auto& s : expected) { if (!exp_str.empty()) exp_str += ","; exp_str += s; }
        std::cout << "  " << (pass ? "PASS" : "FAIL") << ": " << stem(img_path)
                  << " -> [" << actual_str << "]" << (pass ? "" : " (expected [" + exp_str + "])") << "\n";
        EXPECT_EQ(to_endpoints(actual), to_endpoints(expected)) << "Failed on " << img_path;
    }
    std::cout << "PASS: Accurately detected yellow arrows in all " << files.size() << " images.\n";
}

#endif // TEST_YELLOW_ARROWS

// ─── MISALIGNED PIECE (HOVER BOX) ────────────────────────────────────────────
#if TEST_MISALIGNED_PIECE

TEST_F(DetectorsTest, MisalignedPiece) {
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "sample_misaligned_piece").string();
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    std::string debug_dir = "debug_screenshots/misaligned_pieces";
    std::filesystem::create_directories(debug_dir);

    std::cout << "\nRunning unit tests on misaligned piece images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string expected = stem(img_path);
        BoardGeometry img_geo = locate_board(img, board_);
        std::string actual = find_misaligned_piece(img, board_, img_geo);
        bool pass = (actual == expected);
        std::cout << "  " << (pass ? "PASS" : "FAIL") << ": " << stem(img_path)
                  << " -> " << (actual.empty() ? "(none)" : actual)
                  << (pass ? "" : " (expected " + expected + ")") << "\n";
        EXPECT_EQ(actual, expected) << "Failed on " << img_path;
    }
    std::cout << "PASS: Accurately detected misaligned pieces in all " << files.size() << " images.\n";
}

#endif // TEST_MISALIGNED_PIECE

// ─── GAME CLOCKS ─────────────────────────────────────────────────────────────
#if TEST_GAME_CLOCKS

TEST_F(DetectorsTest, GameClocks) {
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "sample_clock_changes").string();
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    std::string debug_dir = "debug_screenshots/game_clocks";
    std::filesystem::create_directories(debug_dir);

    std::cout << "\nRunning unit tests on game clocks...\n";
    int passed = 0, failed = 0;
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string base = stem(img_path);
        std::vector<std::string> parts;
        std::string token;
        for (char c : base) {
            if (c == '_') { parts.push_back(token); token.clear(); }
            else token += c;
        }
        parts.push_back(token);
        if (parts.size() < 3) {
            std::cout << "  SKIP: " << stem(img_path) << " (bad filename format)\n";
            continue;
        }

        std::string expected_active = parts[0];
        std::string expected_white = parts[1];
        std::string expected_black = parts[2];
        for (auto& c : expected_white) if (c == '-') c = ':';
        for (auto& c : expected_black) if (c == '-') c = ':';

        BoardGeometry img_geo = locate_board(img, board_);
        if (img_geo.bw == 0 || img_geo.bh == 0) {
            std::cout << "  SKIP: " << stem(img_path) << " (board not found)\n";
            continue;
        }

        ClockState state = extract_clocks(img, board_, img_geo);
        bool player_ok = (state.active_player == expected_active);
        bool white_ok = (state.white_time == expected_white);
        bool black_ok = (state.black_time == expected_black);
        bool pass = player_ok && white_ok && black_ok;

        // Draw debug boxes for where we searched for the clocks
        cv::Mat debug_img = img.clone();
        int roi_x1 = std::max(0, static_cast<int>(img_geo.bx + img_geo.bw * 0.70));
        int roi_x2 = std::min(debug_img.cols, static_cast<int>(img_geo.bx + img_geo.bw));

        int top_roi_y1 = std::max(0, static_cast<int>(img_geo.by - img_geo.sq_h * 0.40));
        int top_roi_y2 = std::max(0, static_cast<int>(img_geo.by - img_geo.sq_h * 0.08));
        int bot_roi_y1 = std::min(debug_img.rows, static_cast<int>(img_geo.by + img_geo.bh + img_geo.sq_h * 0.18));
        int bot_roi_y2 = std::min(debug_img.rows, static_cast<int>(img_geo.by + img_geo.bh + img_geo.sq_h * 0.58));

        cv::rectangle(debug_img, cv::Point(roi_x1, top_roi_y1), cv::Point(roi_x2, top_roi_y2), cv::Scalar(0, 0, 255), 2); // Red for top (black clock)
        cv::rectangle(debug_img, cv::Point(roi_x1, bot_roi_y1), cv::Point(roi_x2, bot_roi_y2), cv::Scalar(0, 255, 0), 2); // Green for bot (white clock)
        std::string debug_path = (std::filesystem::path(debug_dir) / (stem(img_path) + "_boxes.png")).string();
        cv::imwrite(debug_path, debug_img);

        std::cout << "  " << (pass ? "PASS" : "FAIL") << ": " << stem(img_path)
                  << " -> active=" << (state.active_player.empty() ? "(none)" : state.active_player)
                  << ", white=" << (state.white_time.empty() ? "(none)" : state.white_time)
                  << ", black=" << (state.black_time.empty() ? "(none)" : state.black_time);
        if (!pass) {
            std::cout << " (expected active=" << expected_active
                      << ", white=" << expected_white << ", black=" << expected_black << ")";
            ++failed;
        } else {
            ++passed;
        }
        std::cout << "\n";

        EXPECT_TRUE(player_ok) << "Failed on " << img_path << ": active player mismatch";
        EXPECT_TRUE(white_ok) << "Failed on " << img_path << ": white time '" << state.white_time << "' != '" << expected_white << "'";
        EXPECT_TRUE(black_ok) << "Failed on " << img_path << ": black time '" << state.black_time << "' != '" << expected_black << "'";
    }
    std::cout << (failed == 0 ? "PASS" : "FAIL") << ": " << passed << "/" << (passed + failed) << " clock tests passed.\n";
}

#endif // TEST_GAME_CLOCKS

// ─── SMOKE TEST: CONSTRUCTOR VALIDATION ──────────────────────────────────────
#if TEST_CONSTRUCTOR_THROWS

TEST_F(DetectorsTest, ConstructorThrowsOnMissingAsset) {
    EXPECT_THROW(ChessVideoExtractor("nonexistent.png"), std::runtime_error);
}

#endif // TEST_CONSTRUCTOR_THROWS

// ─── INTEGRATION: 7 PLIES EXTRACTION ─────────────────────────────────────────
#if TEST_7_PLIES_EXTRACTION

TEST_F(DetectorsTest, SevenPliesExtraction) {
    const std::string video_path = (std::filesystem::path(assets_dir_) / "sample_games_short" / "7 plies" / "7 plies.mp4").string();

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }

    std::cout << "\nRunning integration test on 7 plies video...\n";

    IntegrationTestResult result;
    result.name = "7 Plies Extraction";
    result.video_file = "7 plies.mp4";
    result.video_duration_sec = get_video_duration(video_path);

    auto t_start = std::chrono::steady_clock::now();

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "test_7_plies");

    result.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    result.plies_extracted = static_cast<int>(data.moves.size());

    const std::string pgn_path = (std::filesystem::path(assets_dir_) / "sample_games_short" / "7 plies" / "7 plies.pgn").string();
    ASSERT_TRUE(std::filesystem::exists(pgn_path)) << "PGN not found: " << pgn_path;

    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    std::vector<std::string> expected_moves = expected_data.main_line;
    std::multiset<std::string> expected_all(expected_data.all_moves.begin(), expected_data.all_moves.end());
    std::cout << "  Loaded expected baseline from PGN.\n";

    result.plies_expected = static_cast<int>(expected_moves.size());

    std::cout << "  Expected Main Line (" << expected_moves.size() << "): ";
    for (const auto& m : expected_moves) std::cout << m << " ";
    std::cout << "\n";

    std::cout << "  Extracted Main Line (" << data.moves.size() << "): ";
    for (const auto& m : data.moves) std::cout << m << " ";
    std::cout << "\n";

    const size_t first_mismatch = first_main_line_mismatch(expected_moves, data.moves);
    if (first_mismatch < data.moves.size() || first_mismatch < expected_moves.size()) {
        std::cout << "  First main-line mismatch at ply " << (first_mismatch + 1)
                  << ": expected "
                  << (first_mismatch < expected_moves.size() ? expected_moves[first_mismatch] : "(none)")
                  << ", extracted "
                  << (first_mismatch < data.moves.size() ? data.moves[first_mismatch] : "(none)")
                  << "\n";
    }

    std::multiset<std::string> extracted_all = extract_all_moves_multiset(data);

    const bool main_line_passed = data.moves == expected_moves;
    const bool complete_output_passed = extracted_all == expected_all;
    const bool invariants_passed = verify_game_data_invariants(data);
    write_first_divergence_report(
        result.name, video_path, expected_moves, data, first_mismatch,
        main_line_passed, complete_output_passed,
        !main_line_passed ? "" :
        (complete_output_passed ? "" : "variation_or_move_set_mismatch"));
    result.passed = main_line_passed && complete_output_passed && invariants_passed;
    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted main line has " << data.moves.size() << " moves, expected " << expected_moves.size();
    EXPECT_EQ(extracted_all, expected_all)
        << "Mismatch in total extracted moves (including variations). App may have hallucinated or missed analysis lines.";

    if (result.passed) {
        std::cout << "PASS: Extracted moves perfectly match the expected " << expected_moves.size() << " plies from the PGN.\n";
    }

    g_test_results.push_back(result);
    print_test_summary();
}

#endif // TEST_7_PLIES_EXTRACTION

// ─── INTEGRATION: MEDIUM GAME WITH REVERT ─────────────────────────────────────
#if TEST_MEDIUM_GAME_REVERT

TEST_F(DetectorsTest, MediumGameWithRevert) {
    const std::string video_path = (std::filesystem::path(assets_dir_) / "sample_games_medium" / "medium_game_with_analysis_line_and_revert.mp4").string();
    const std::string pgn_path = (std::filesystem::path(assets_dir_) / "sample_games_medium" / "game.pgn").string();

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }
    ASSERT_TRUE(std::filesystem::exists(pgn_path)) << "PGN not found: " << pgn_path;

    std::cout << "\nRunning integration test on medium game with revert...\n";

    IntegrationTestResult result;
    result.name = "Medium Game + Revert";
    result.video_file = "medium_game_with_revert.mp4";
    result.video_duration_sec = get_video_duration(video_path);

    auto t_start = std::chrono::steady_clock::now();

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "test_medium_revert");

    result.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    result.plies_extracted = static_cast<int>(data.moves.size());

    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    std::vector<std::string> expected_moves = expected_data.main_line;
    std::multiset<std::string> expected_all(expected_data.all_moves.begin(), expected_data.all_moves.end());
    std::cout << "  Loaded expected baseline from PGN.\n";

    result.plies_expected = static_cast<int>(expected_moves.size());
    result.reverts_detected = 1; // This test is known to have one analysis revert

    std::cout << "  Expected Main Line (" << expected_moves.size() << "): ";
    for (const auto& m : expected_moves) std::cout << m << " ";
    std::cout << "\n";

    std::cout << "  Extracted Main Line (" << data.moves.size() << "): ";
    for (const auto& m : data.moves) std::cout << m << " ";
    std::cout << "\n";

    std::multiset<std::string> extracted_all = extract_all_moves_multiset(data);

    const size_t first_mismatch = first_main_line_mismatch(expected_moves, data.moves);
    const bool main_line_passed = data.moves == expected_moves;
    const bool complete_output_passed = extracted_all == expected_all;
    const bool invariants_passed = verify_game_data_invariants(data);
    write_first_divergence_report(
        result.name, video_path, expected_moves, data, first_mismatch,
        main_line_passed, complete_output_passed,
        !main_line_passed ? "" :
        (complete_output_passed ? "" : "variation_or_move_set_mismatch"));
    result.passed = main_line_passed && complete_output_passed && invariants_passed;
    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted main line has " << data.moves.size() << " moves, expected " << expected_moves.size();
    EXPECT_EQ(extracted_all, expected_all)
        << "Mismatch in total extracted moves (including variations). App may have hallucinated or missed analysis lines.";

    if (result.passed) {
        std::cout << "PASS: Extracted moves perfectly match the expected " << expected_moves.size()
                  << " moves from the PGN, correctly handling analysis line revert.\n";
    }

    g_test_results.push_back(result);
    print_test_summary();
}

#endif // TEST_MEDIUM_GAME_REVERT

// ─── INTEGRATION: FULL GAME 1 EXTRACTION ─────────────────────────────────────
#if TEST_FULL_GAME_1_EXTRACTION

TEST_F(DetectorsTest, FullGame1Extraction) {
    const std::string dir_path = (std::filesystem::path(assets_dir_) / "sample_games_full" / "game_1").string();
    auto vid_files = list_files(dir_path, {".mp4", ".mkv", ".webm", ".avi"});
    
    if (vid_files.empty()) {
        GTEST_SKIP() << "Video not found in: " << dir_path;
    }
    
    const std::string video_path = vid_files[0];
    const std::string pgn_path = (std::filesystem::path(dir_path) / "game.pgn").string();

    if (!std::filesystem::exists(pgn_path)) {
        GTEST_SKIP() << "PGN not found: " << pgn_path;
    }

    std::cout << "\nRunning integration test on Full Game 1...\n";

    IntegrationTestResult result;
    result.name = "Full Game 1 Extraction";
    result.video_file = std::filesystem::path(video_path).filename().string();
    result.video_duration_sec = get_video_duration(video_path);

    auto t_start = std::chrono::steady_clock::now();

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "test_full_game_1");
    result.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    result.plies_extracted = static_cast<int>(data.moves.size());

    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    std::vector<std::string> expected_moves = expected_data.main_line;
    std::multiset<std::string> expected_all(expected_data.all_moves.begin(), expected_data.all_moves.end());
    std::cout << "  Loaded expected baseline from PGN.\n";

    result.plies_expected = static_cast<int>(expected_moves.size());
    result.reverts_detected = 0; // Can be updated if GameData tracks total reverts in the future

    std::cout << "  Expected Main Line (" << expected_moves.size() << "): ";
    for (const auto& m : expected_moves) std::cout << m << " ";
    std::cout << "\n";

    std::cout << "  Extracted Main Line (" << data.moves.size() << "): ";
    for (const auto& m : data.moves) std::cout << m << " ";
    std::cout << "\n";

    const size_t first_mismatch = first_main_line_mismatch(expected_moves, data.moves);
    if (first_mismatch < data.moves.size() || first_mismatch < expected_moves.size()) {
        std::cout << "  First main-line mismatch at ply " << (first_mismatch + 1)
                  << ": expected "
                  << (first_mismatch < expected_moves.size() ? expected_moves[first_mismatch] : "(none)")
                  << ", extracted "
                  << (first_mismatch < data.moves.size() ? data.moves[first_mismatch] : "(none)")
                  << "\n";
    }

    std::multiset<std::string> extracted_all = extract_all_moves_multiset(data);
    std::multiset<std::string> detected_timeline;
    for (const std::string& move : data.video_moves) {
        if (move != "REVERT") {
            detected_timeline.insert(move);
        }
    }

    const bool main_line_passed = data.moves == expected_moves;
    const bool move_set_passed = extracted_all == expected_all;
    const bool timeline_passed = detected_timeline == expected_all;
    const bool invariants_passed = verify_game_data_invariants(data);
    bool moves_passed = main_line_passed && move_set_passed && timeline_passed && invariants_passed;
    write_first_divergence_report(
        result.name, video_path, expected_moves, data, first_mismatch,
        main_line_passed, moves_passed,
        !timeline_passed ? "accepted_timeline_mismatch" :
        (!move_set_passed ? "variation_or_move_set_mismatch" : ""));
    if (!moves_passed) {
        std::ofstream variation_dump("full_game_variations_debug.txt", std::ios::trunc);
        if (variation_dump.is_open()) {
            for (const auto& [parent_ply, variations] : data.variations) {
                for (const auto& variation : variations) {
                    variation_dump << parent_ply << "\t";
                    for (size_t i = 0; i < variation.moves.size(); ++i) {
                        if (i != 0) variation_dump << ',';
                        variation_dump << variation.moves[i];
                    }
                    variation_dump << "\n";
                }
            }
        }
        std::cout << "  Expected variation moves from PGN parser:\n";
        for (const auto& move : expected_data.variations) std::cout << "    " << move << "\n";
        std::cout << "  Expected variation sequences by parent ply:\n";
        for (const auto& [parent_ply, moves] : expected_data.variation_sequences) {
            std::cout << "    parent " << parent_ply << ": ";
            for (const auto& move : moves) std::cout << move << " ";
            std::cout << "\n";
        }
        std::cout << "  Extracted variation tree:\n";
        for (const auto& [parent_ply, variations] : data.variations) {
            for (const auto& variation : variations) {
                std::cout << "    parent " << parent_ply << ": ";
                for (const auto& move : variation.moves) std::cout << move << " ";
                std::cout << "\n";
            }
        }
    }
    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted main line has " << data.moves.size() << " moves, expected " << expected_moves.size();
    print_multiset_delta(extracted_all, expected_all, "moves");
    EXPECT_EQ(extracted_all, expected_all)
        << "Mismatch in total extracted moves (including variations). App may have hallucinated or missed analysis lines.";
    print_multiset_delta(detected_timeline, expected_all, "timeline moves");
    EXPECT_EQ(detected_timeline, expected_all)
        << "Mismatch in accepted move timeline. App may have transiently detected moves that are absent from the PGN.";

    bool clocks_passed = verify_clocks(pgn_path, data);
    result.passed = moves_passed && clocks_passed;

    if (result.passed) {
        std::cout << "PASS: Extracted moves and clocks perfectly match the expected PGN, including variation clocks.\n";
    }

    g_test_results.push_back(result);
    print_test_summary();
}

#endif // TEST_FULL_GAME_1_EXTRACTION

// ─── INTEGRATION: CLOCK TIMES EXTRACTION ─────────────────────────────────────
#if TEST_INTEGRATION_CLOCK_TIMES

TEST_F(DetectorsTest, IntegrationClockTimes) {
    const std::string dir_path = (std::filesystem::path(assets_dir_) / "test_clock_times").string();
    auto vid_files = list_files(dir_path, {".mp4", ".mkv", ".webm", ".avi"});
    
    if (vid_files.empty()) {
        GTEST_SKIP() << "Video not found in: " << dir_path;
    }
    
    const std::string video_path = vid_files[0];
    auto pgn_files = list_files(dir_path, {".pgn"});
    if (pgn_files.empty()) {
        GTEST_SKIP() << "PGN not found in: " << dir_path;
    }
    const std::string pgn_path = pgn_files[0];

    std::cout << "\nRunning integration test on clock times...\n";

    IntegrationTestResult result;
    result.name = "Clock Times Integration";
    result.video_file = std::filesystem::path(video_path).filename().string();
    result.video_duration_sec = get_video_duration(video_path);

    auto t_start = std::chrono::steady_clock::now();

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "test_clock_times");

    result.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    result.plies_extracted = static_cast<int>(data.moves.size());

    // Extract and verify expected moves from PGN
    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    std::vector<std::string> expected_moves = expected_data.main_line;
    std::multiset<std::string> expected_all(expected_data.all_moves.begin(), expected_data.all_moves.end());
    std::cout << "  Loaded expected moves from PGN.\n";
    result.plies_expected = static_cast<int>(expected_moves.size());

    std::cout << "  Expected Main Line (" << expected_moves.size() << "): ";
    for (const auto& m : expected_moves) std::cout << m << " ";
    std::cout << "\n";

    std::cout << "  Extracted Main Line (" << data.moves.size() << "): ";
    for (const auto& m : data.moves) std::cout << m << " ";
    std::cout << "\n";

    std::multiset<std::string> extracted_all = extract_all_moves_multiset(data);

    bool moves_passed = (data.moves == expected_moves) && (extracted_all == expected_all);
    const bool invariants_passed = verify_game_data_invariants(data);
    moves_passed = moves_passed && invariants_passed;
    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted main line has " << data.moves.size() << " moves, expected " << expected_moves.size();
    EXPECT_EQ(extracted_all, expected_all)
        << "Mismatch in total extracted moves (including variations). App may have hallucinated or missed analysis lines.";

    bool clocks_passed = verify_clocks(pgn_path, data);

    result.passed = moves_passed && clocks_passed;

    if (result.passed) {
        std::cout << "PASS: Extracted moves and clocks perfectly match the expected PGN.\n";
    }

    g_test_results.push_back(result);
    print_test_summary();
}

#endif // TEST_INTEGRATION_CLOCK_TIMES

// ─── MEMORY LIMIT BEHAVIOR ───────────────────────────────────────────────────
#if TEST_MEMORY_LIMIT

TEST_F(DetectorsTest, MemoryLimitWorkerCount) {
    const std::string video_path = (std::filesystem::path(assets_dir_) / "sample_games_medium" / "medium_game_with_analysis_line_and_revert.mp4").string();

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }

    std::cout << "\nRunning unit test on memory limit worker count...\n";

    // 250MB limit should restrict it to max 1 worker.
    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None, 250);
    
    int detected_workers = -1;
    extractor.set_progress_callback([&](int percent, const std::string& msg) {
        if (msg.find("Launching Map-Reduce visual extraction") != std::string::npos) {
            size_t start = msg.find("(") + 1;
            size_t end = msg.find(" workers");
            if (start != std::string::npos && end != std::string::npos) {
                detected_workers = std::stoi(msg.substr(start, end - start));
            }
        }
    });

    // Run extraction with immediate cancellation to avoid waiting for the whole video
    std::atomic<bool> cancel{true};
    
    try {
        extractor.extract_moves_from_video(video_path, "test_mem_limit", &cancel);
    } catch (...) {
        // It might throw or exit cleanly upon cancellation, either is fine as long as we got the log
    }

    EXPECT_NE(detected_workers, -1) << "Did not find Map-Reduce launch log message.";
    EXPECT_EQ(detected_workers, 1) << "Memory limit of 250MB should restrict worker count to 1.";
}

#endif // TEST_MEMORY_LIMIT

// ─── CACHE CORRECTNESS TEST ──────────────────────────────────────────────────
#if TEST_CACHE_CORRECTNESS

TEST_F(DetectorsTest, CacheCorrectness) {
    const std::string video_path = (std::filesystem::path(assets_dir_) / "sample_games_short" / "7 plies" / "7 plies.mp4").string();

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }

    std::cout << "\nRunning unit test on board caching behavior...\n";

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    
    int cache_loads = 0;
    int multi_pass_searches = 0;

    extractor.set_progress_callback([&](int percent, const std::string& msg) {
        if (msg.find("Loaded exact board scale from cache") != std::string::npos) {
            cache_loads++;
        }
        if (msg.find("Performing multi-pass template matching") != std::string::npos) {
            multi_pass_searches++;
        }
    });

    std::atomic<bool> cancel{true};
    
    // First run might be a hit or miss depending on the environment,
    // but running it twice guarantees the second run is a hit if caching works.
    try { extractor.extract_moves_from_video(video_path, "test_cache", &cancel); } catch (...) {}
    
    cache_loads = 0;
    multi_pass_searches = 0;
    
    // Second run: must hit the cache
    try { extractor.extract_moves_from_video(video_path, "test_cache", &cancel); } catch (...) {}

    EXPECT_EQ(cache_loads, 1) << "Second run did not load from cache.";
    EXPECT_EQ(multi_pass_searches, 0) << "Second run performed multi-pass search instead of using cache.";
}

#endif // TEST_CACHE_CORRECTNESS

} // namespace cta
